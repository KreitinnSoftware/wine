/*
 * Copyright 2008 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "mshtml_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

typedef struct {
    HTMLFrameBase framebase;

    LONG ref;

    nsIDOMHTMLIFrameElement *nsiframe;
} HTMLIFrame;

#define HTMLIFRAME_NODE_THIS(iface) DEFINE_THIS2(HTMLIFrame, framebase.element.node, iface)

static HRESULT HTMLIFrame_QI(HTMLDOMNode *iface, REFIID riid, void **ppv)
{
    HTMLIFrame *This = HTMLIFRAME_NODE_THIS(iface);

    return HTMLFrameBase_QI(&This->framebase, riid, ppv);
}

static void HTMLIFrame_destructor(HTMLDOMNode *iface)
{
    HTMLIFrame *This = HTMLIFRAME_NODE_THIS(iface);

    if(This->nsiframe)
        nsIDOMHTMLIFrameElement_Release(This->nsiframe);

    HTMLFrameBase_destructor(&This->framebase);
}

#undef HTMLIFRAME_NODE_THIS

static const NodeImplVtbl HTMLIFrameImplVtbl = {
    HTMLIFrame_QI,
    HTMLIFrame_destructor
};

static const tid_t HTMLIFrame_iface_tids[] = {
    IHTMLDOMNode_tid,
    IHTMLDOMNode2_tid,
    IHTMLElement_tid,
    IHTMLElement2_tid,
    IHTMLElement3_tid,
    IHTMLFrameBase_tid,
    IHTMLFrameBase2_tid,
    0
};

static dispex_static_data_t HTMLIFrame_dispex = {
    NULL,
    DispHTMLIFrame_tid,
    NULL,
    HTMLIFrame_iface_tids
};

static HTMLWindow *get_content_window(nsIDOMHTMLIFrameElement *nsiframe)
{
    HTMLWindow *ret;
    nsIDOMWindow *nswindow;
    nsIDOMDocument *nsdoc;
    nsresult nsres;

    nsres = nsIDOMHTMLIFrameElement_GetContentDocument(nsiframe, &nsdoc);
    if(NS_FAILED(nsres)) {
        ERR("GetContentDocument failed: %08x\n", nsres);
        return NULL;
    }

    if(!nsdoc) {
        FIXME("NULL contentDocument\n");
        return NULL;
    }

    nswindow = get_nsdoc_window(nsdoc);
    nsIDOMDocument_Release(nsdoc);
    if(!nswindow)
        return NULL;

    ret = nswindow_to_window(nswindow);
    nsIDOMWindow_Release(nswindow);
    if(!ret)
        ERR("Could not get window object\n");

    return ret;
}

HTMLElement *HTMLIFrame_Create(HTMLDocumentNode *doc, nsIDOMHTMLElement *nselem, HTMLWindow *content_window)
{
    HTMLIFrame *ret;
    nsresult nsres;

    ret = heap_alloc_zero(sizeof(HTMLIFrame));

    ret->framebase.element.node.vtbl = &HTMLIFrameImplVtbl;

    nsres = nsIDOMHTMLElement_QueryInterface(nselem, &IID_nsIDOMHTMLIFrameElement, (void**)&ret->nsiframe);
    if(NS_FAILED(nsres))
        ERR("Could not get nsIDOMHTMLIFrameElement iface: %08x\n", nsres);

    if(!content_window)
        content_window = get_content_window(ret->nsiframe);

    HTMLFrameBase_Init(&ret->framebase, doc, nselem, content_window, &HTMLIFrame_dispex);

    return &ret->framebase.element;
}
