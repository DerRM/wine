/*
 * Copyright 2017 Hans Leidekker for CodeWeavers
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

#include "windef.h"
#include "winbase.h"
#include "ws2tcpip.h"
#include "webservices.h"

#include "wine/debug.h"
#include "wine/list.h"
#include "wine/unicode.h"
#include "webservices_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(webservices);

static BOOL winsock_loaded;

static BOOL WINAPI winsock_startup( INIT_ONCE *once, void *param, void **ctx )
{
    int ret;
    WSADATA data;
    if (!(ret = WSAStartup( MAKEWORD(1,1), &data ))) winsock_loaded = TRUE;
    else ERR( "WSAStartup failed: %d\n", ret );
    return TRUE;
}

static void winsock_init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, winsock_startup, NULL, NULL );
}

static const struct prop_desc listener_props[] =
{
    { sizeof(ULONG), FALSE },                               /* WS_LISTENER_PROPERTY_LISTEN_BACKLOG */
    { sizeof(WS_IP_VERSION), FALSE },                       /* WS_LISTENER_PROPERTY_IP_VERSION */
    { sizeof(WS_LISTENER_STATE), TRUE },                    /* WS_LISTENER_PROPERTY_STATE */
    { sizeof(WS_CALLBACK_MODEL), FALSE },                   /* WS_LISTENER_PROPERTY_ASYNC_CALLBACK_MODEL */
    { sizeof(WS_CHANNEL_TYPE), TRUE },                      /* WS_LISTENER_PROPERTY_CHANNEL_TYPE */
    { sizeof(WS_CHANNEL_BINDING), TRUE },                   /* WS_LISTENER_PROPERTY_CHANNEL_BINDING */
    { sizeof(ULONG), FALSE },                               /* WS_LISTENER_PROPERTY_CONNECT_TIMEOUT */
    { sizeof(BOOL), FALSE },                                /* WS_LISTENER_PROPERTY_IS_MULTICAST */
    { 0, FALSE },                                           /* WS_LISTENER_PROPERTY_MULTICAST_INTERFACES */
    { sizeof(BOOL), FALSE },                                /* WS_LISTENER_PROPERTY_MULTICAST_LOOPBACK */
    { sizeof(ULONG), FALSE },                               /* WS_LISTENER_PROPERTY_CLOSE_TIMEOUT */
    { sizeof(ULONG), FALSE },                               /* WS_LISTENER_PROPERTY_TO_HEADER_MATCHING_OPTIONS */
    { sizeof(ULONG), FALSE },                               /* WS_LISTENER_PROPERTY_TRANSPORT_URL_MATCHING_OPTIONS */
    { sizeof(WS_CUSTOM_LISTENER_CALLBACKS), FALSE },        /* WS_LISTENER_PROPERTY_CUSTOM_LISTENER_CALLBACKS */
    { 0, FALSE },                                           /* WS_LISTENER_PROPERTY_CUSTOM_LISTENER_PARAMETERS */
    { sizeof(void *), TRUE },                               /* WS_LISTENER_PROPERTY_CUSTOM_LISTENER_INSTANCE */
    { sizeof(WS_DISALLOWED_USER_AGENT_SUBSTRINGS), FALSE }  /* WS_LISTENER_PROPERTY_DISALLOWED_USER_AGENT */
};

struct listener
{
    ULONG                   magic;
    CRITICAL_SECTION        cs;
    WS_CHANNEL_TYPE         type;
    WS_CHANNEL_BINDING      binding;
    WS_LISTENER_STATE       state;
    SOCKET                  socket;
    ULONG                   prop_count;
    struct prop             prop[sizeof(listener_props)/sizeof(listener_props[0])];
};

#define LISTENER_MAGIC (('L' << 24) | ('I' << 16) | ('S' << 8) | 'T')

static struct listener *alloc_listener(void)
{
    static const ULONG count = sizeof(listener_props)/sizeof(listener_props[0]);
    struct listener *ret;
    ULONG size = sizeof(*ret) + prop_size( listener_props, count );

    if (!(ret = heap_alloc_zero( size ))) return NULL;

    ret->magic      = LISTENER_MAGIC;
    InitializeCriticalSection( &ret->cs );
    ret->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": listener.cs");

    prop_init( listener_props, count, ret->prop, &ret[1] );
    ret->prop_count = count;
    return ret;
}

static void reset_listener( struct listener *listener )
{
    closesocket( listener->socket );
    listener->socket = -1;
    listener->state  = WS_LISTENER_STATE_CREATED;
}

static void free_listener( struct listener *listener )
{
    reset_listener( listener );
    listener->cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection( &listener->cs );
    heap_free( listener );
}

static HRESULT create_listener( WS_CHANNEL_TYPE type, WS_CHANNEL_BINDING binding,
                                const WS_LISTENER_PROPERTY *properties, ULONG count, struct listener **ret )
{
    struct listener *listener;
    HRESULT hr;
    ULONG i;

    if (!(listener = alloc_listener())) return E_OUTOFMEMORY;

    for (i = 0; i < count; i++)
    {
        hr = prop_set( listener->prop, listener->prop_count, properties[i].id, properties[i].value,
                       properties[i].valueSize );
        if (hr != S_OK)
        {
            free_listener( listener );
            return hr;
        }
    }

    listener->type    = type;
    listener->binding = binding;
    listener->socket  = -1;

    *ret = listener;
    return S_OK;
}

/**************************************************************************
 *          WsCreateListener		[webservices.@]
 */
HRESULT WINAPI WsCreateListener( WS_CHANNEL_TYPE type, WS_CHANNEL_BINDING binding,
                                 const WS_LISTENER_PROPERTY *properties, ULONG count,
                                 const WS_SECURITY_DESCRIPTION *desc, WS_LISTENER **handle,
                                 WS_ERROR *error )
{
    struct listener *listener;
    HRESULT hr;

    TRACE( "%u %u %p %u %p %p %p\n", type, binding, properties, count, desc, handle, error );
    if (error) FIXME( "ignoring error parameter\n" );
    if (desc) FIXME( "ignoring security description\n" );

    if (!handle) return E_INVALIDARG;

    if (type != WS_CHANNEL_TYPE_DUPLEX_SESSION)
    {
        FIXME( "channel type %u not implemented\n", type );
        return E_NOTIMPL;
    }
    if (binding != WS_TCP_CHANNEL_BINDING)
    {
        FIXME( "channel binding %u not implemented\n", binding );
        return E_NOTIMPL;
    }

    if ((hr = create_listener( type, binding, properties, count, &listener )) != S_OK) return hr;

    *handle = (WS_LISTENER *)listener;
    return S_OK;
}

/**************************************************************************
 *          WsFreeListener		[webservices.@]
 */
void WINAPI WsFreeListener( WS_LISTENER *handle )
{
    struct listener *listener = (struct listener *)handle;

    TRACE( "%p\n", handle );

    if (!listener) return;

    EnterCriticalSection( &listener->cs );

    if (listener->magic != LISTENER_MAGIC)
    {
        LeaveCriticalSection( &listener->cs );
        return;
    }

    listener->magic = 0;

    LeaveCriticalSection( &listener->cs );
    free_listener( listener );
}

static HRESULT resolve_hostname( const WCHAR *host, USHORT port, struct sockaddr *addr, int *addr_len )
{
    static const WCHAR fmtW[] = {'%','u',0};
    WCHAR service[6];
    ADDRINFOW *res, *info;
    HRESULT hr = WS_E_ADDRESS_NOT_AVAILABLE;

    *addr_len = 0;
    sprintfW( service, fmtW, port );
    if (GetAddrInfoW( host, service, NULL, &res )) return HRESULT_FROM_WIN32( WSAGetLastError() );

    info = res;
    while (info && info->ai_family != AF_INET && info->ai_family != AF_INET6) info = info->ai_next;
    if (info)
    {
        memcpy( addr, info->ai_addr, info->ai_addrlen );
        *addr_len = info->ai_addrlen;
        hr = S_OK;
    }

    FreeAddrInfoW( res );
    return hr;
}

static HRESULT parse_url( const WS_STRING *str, WCHAR **host, USHORT *port )
{
    WS_HEAP *heap;
    WS_NETTCP_URL *url;
    HRESULT hr;

    if ((hr = WsCreateHeap( 1 << 8, 0, NULL, 0, &heap, NULL )) != S_OK) return hr;
    if ((hr = WsDecodeUrl( str, 0, heap, (WS_URL **)&url, NULL )) != S_OK)
    {
        WsFreeHeap( heap );
        return hr;
    }

    if (url->host.length == 1 && (url->host.chars[0] == '+' || url->host.chars[0] == '*')) *host = NULL;
    else
    {
        if (!(*host = heap_alloc( (url->host.length + 1) * sizeof(WCHAR) )))
        {
            WsFreeHeap( heap );
            return E_OUTOFMEMORY;
        }
        memcpy( *host, url->host.chars, url->host.length * sizeof(WCHAR) );
        (*host)[url->host.length] = 0;
    }
    *port = url->port;

    WsFreeHeap( heap );
    return hr;
}

static HRESULT open_listener( struct listener *listener, const WS_STRING *url )
{
    struct sockaddr_storage storage;
    struct sockaddr *addr = (struct sockaddr *)&storage;
    int addr_len;
    WCHAR *host;
    USHORT port;
    HRESULT hr;

    if ((hr = parse_url( url, &host, &port )) != S_OK) return hr;

    winsock_init();

    hr = resolve_hostname( host, port, addr, &addr_len );
    heap_free( host );
    if (hr != S_OK) return hr;

    if ((listener->socket = socket( addr->sa_family, SOCK_STREAM, 0 )) == -1)
        return HRESULT_FROM_WIN32( WSAGetLastError() );

    if (bind( listener->socket, addr, addr_len ) < 0)
    {
        closesocket( listener->socket );
        listener->socket = -1;
        return HRESULT_FROM_WIN32( WSAGetLastError() );
    }

    if (listen( listener->socket, 0 ) < 0)
    {
        closesocket( listener->socket );
        listener->socket = -1;
        return HRESULT_FROM_WIN32( WSAGetLastError() );
    }

    listener->state = WS_LISTENER_STATE_OPEN;
    return S_OK;
}

/**************************************************************************
 *          WsOpenListener		[webservices.@]
 */
HRESULT WINAPI WsOpenListener( WS_LISTENER *handle, WS_STRING *url, const WS_ASYNC_CONTEXT *ctx, WS_ERROR *error )
{
    struct listener *listener = (struct listener *)handle;
    HRESULT hr;

    TRACE( "%p %s %p %p\n", handle, url ? debugstr_wn(url->chars, url->length) : "null", ctx, error );
    if (error) FIXME( "ignoring error parameter\n" );
    if (ctx) FIXME( "ignoring ctx parameter\n" );

    if (!listener || !url) return E_INVALIDARG;

    EnterCriticalSection( &listener->cs );

    if (listener->magic != LISTENER_MAGIC)
    {
        LeaveCriticalSection( &listener->cs );
        return E_INVALIDARG;
    }

    if (listener->state != WS_LISTENER_STATE_CREATED)
    {
        LeaveCriticalSection( &listener->cs );
        return WS_E_INVALID_OPERATION;
    }

    hr = open_listener( listener, url );

    LeaveCriticalSection( &listener->cs );
    return hr;
}

static void close_listener( struct listener *listener )
{
    reset_listener( listener );
    listener->state = WS_LISTENER_STATE_CLOSED;
}

/**************************************************************************
 *          WsCloseListener		[webservices.@]
 */
HRESULT WINAPI WsCloseListener( WS_LISTENER *handle, const WS_ASYNC_CONTEXT *ctx, WS_ERROR *error )
{
    struct listener *listener = (struct listener *)handle;

    TRACE( "%p %p %p\n", handle, ctx, error );
    if (error) FIXME( "ignoring error parameter\n" );
    if (ctx) FIXME( "ignoring ctx parameter\n" );

    if (!listener) return E_INVALIDARG;

    EnterCriticalSection( &listener->cs );

    if (listener->magic != LISTENER_MAGIC)
    {
        LeaveCriticalSection( &listener->cs );
        return E_INVALIDARG;
    }

    close_listener( listener );

    LeaveCriticalSection( &listener->cs );
    return S_OK;
}

/**************************************************************************
 *          WsGetListenerProperty		[webservices.@]
 */
HRESULT WINAPI WsGetListenerProperty( WS_LISTENER *handle, WS_LISTENER_PROPERTY_ID id, void *buf,
                                      ULONG size, WS_ERROR *error )
{
    struct listener *listener = (struct listener *)handle;
    HRESULT hr = S_OK;

    TRACE( "%p %u %p %u %p\n", handle, id, buf, size, error );
    if (error) FIXME( "ignoring error parameter\n" );

    if (!listener) return E_INVALIDARG;

    EnterCriticalSection( &listener->cs );

    if (listener->magic != LISTENER_MAGIC)
    {
        LeaveCriticalSection( &listener->cs );
        return E_INVALIDARG;
    }

    switch (id)
    {
    case WS_LISTENER_PROPERTY_STATE:
        if (!buf || size != sizeof(listener->state)) hr = E_INVALIDARG;
        else *(WS_LISTENER_STATE *)buf = listener->state;
        break;

    case WS_LISTENER_PROPERTY_CHANNEL_TYPE:
        if (!buf || size != sizeof(listener->type)) hr = E_INVALIDARG;
        else *(WS_CHANNEL_TYPE *)buf = listener->type;
        break;

    case WS_LISTENER_PROPERTY_CHANNEL_BINDING:
        if (!buf || size != sizeof(listener->binding)) hr = E_INVALIDARG;
        else *(WS_CHANNEL_BINDING *)buf = listener->binding;
        break;

    default:
        hr = prop_get( listener->prop, listener->prop_count, id, buf, size );
    }

    LeaveCriticalSection( &listener->cs );
    return hr;
}

/**************************************************************************
 *          WsSetListenerProperty		[webservices.@]
 */
HRESULT WINAPI WsSetListenerProperty( WS_LISTENER *handle, WS_LISTENER_PROPERTY_ID id, const void *value,
                                      ULONG size, WS_ERROR *error )
{
    struct listener *listener = (struct listener *)handle;
    HRESULT hr;

    TRACE( "%p %u %p %u\n", handle, id, value, size );
    if (error) FIXME( "ignoring error parameter\n" );

    if (!listener) return E_INVALIDARG;

    EnterCriticalSection( &listener->cs );

    if (listener->magic != LISTENER_MAGIC)
    {
        LeaveCriticalSection( &listener->cs );
        return E_INVALIDARG;
    }

    hr = prop_set( listener->prop, listener->prop_count, id, value, size );

    LeaveCriticalSection( &listener->cs );
    return hr;
}
