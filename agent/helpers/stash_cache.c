#include <net-snmp/net-snmp-config.h>

#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <net-snmp/agent/stash_cache.h>
#include <net-snmp/agent/stash_to_next.h>

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

extern NetsnmpCacheLoad _netsnmp_stash_cache_load;
extern NetsnmpCacheFree _netsnmp_stash_cache_free;
 
/** @defgroup stash_cache stash_cache: automatically caches data for certain handlers.
 *  This handler caches data in an optimized way which may aleviate
 *  the need for the lower level handlers to perform as much
 *  optimization.  Specifically, somewhere in the lower level handlers
 *  must be a handler that supports the MODE_GET_STASH operation.
 *  Note that the table_iterator helper supports this.
 *  @ingroup handler
 *  @{
 */

netsnmp_stash_cache_info *
netsnmp_get_new_stash_cache(void)
{
    netsnmp_stash_cache_info *cinfo;

    cinfo = SNMP_MALLOC_TYPEDEF(netsnmp_stash_cache_info);
    cinfo->cache_length = 30;
    return cinfo;
}

/** returns a single stash_cache handler that can be injected into a given
 *  handler chain, but *only* if that handler chain explicitly supports
 *  stash cache processing.
 */
netsnmp_mib_handler *
netsnmp_get_bare_stash_cache_handler(void)
{
    netsnmp_mib_handler *handler;
    netsnmp_cache       *cinfo;

    cinfo = netsnmp_cache_create( 30, _netsnmp_stash_cache_load,
                                 _netsnmp_stash_cache_free, NULL, 0 );

    if (!cinfo)
        return NULL;

    handler = netsnmp_cache_handler_get( cinfo );
    if (!handler) {
        free(cinfo);
        return NULL;
    }

    handler->next = netsnmp_create_handler("stash_cache", netsnmp_stash_cache_helper);
    if (!handler->next) {
        free(cinfo);
        return NULL;
    }

    handler->myvoid = cinfo;

    return handler;
}

/** returns a stash_cache handler sub-chain that can be injected into a given
 *  (arbitrary) handler chain.
 */
netsnmp_mib_handler *
netsnmp_get_stash_cache_handler(void)
{
    netsnmp_mib_handler *handler = netsnmp_get_bare_stash_cache_handler();
    if (handler && handler->next) {
        handler->next->next = netsnmp_get_stash_to_next_handler();
    }
    return handler;
}

/** extracts a pointer to the stash_cache info from the reqinfo structure. */
netsnmp_oid_stash_node  **
netsnmp_extract_stash_cache(netsnmp_agent_request_info *reqinfo)
{
    return netsnmp_agent_get_list_data(reqinfo, STASH_CACHE_NAME);
}


/** @internal Implements the stash_cache handler */
int
netsnmp_stash_cache_helper(netsnmp_mib_handler *handler,
                           netsnmp_handler_registration *reginfo,
                           netsnmp_agent_request_info *reqinfo,
                           netsnmp_request_info *requests)
{
    netsnmp_cache            *cache;
    netsnmp_stash_cache_info *cinfo;
    netsnmp_oid_stash_node   *cnode;
    netsnmp_variable_list    *cdata;
    netsnmp_request_info     *request;

    DEBUGMSGTL(("helper:stash_cache", "Got request\n"));

    cache = netsnmp_cache_reqinfo_extract( reqinfo, reginfo->handlerName );
    cinfo = (netsnmp_stash_cache_info *) cache->magic;

    switch (reqinfo->mode) {

    case MODE_GET:
        DEBUGMSGTL(("helper:stash_cache", "Processing GET request\n"));
        for(request = requests; request; request = request->next) {
            cdata =
                netsnmp_oid_stash_get_data(cinfo->cache,
                                           requests->requestvb->name,
                                           requests->requestvb->name_length);
            if (cdata && cdata->val.string && cdata->val_len) {
                DEBUGMSGTL(("helper:stash_cache", "Found cached GET varbind\n"));
                DEBUGMSGOID(("helper:stash_cache", cdata->name, cdata->name_length));
                DEBUGMSG(("helper:stash_cache", "\n"));
                snmp_set_var_typed_value(request->requestvb, cdata->type,
                                         cdata->val.string, cdata->val_len);
            }
        }
        return SNMP_ERR_NOERROR;
        break;

    case MODE_GETNEXT:
        DEBUGMSGTL(("helper:stash_cache", "Processing GETNEXT request\n"));
        for(request = requests; request; request = request->next) {
            cnode =
                netsnmp_oid_stash_getnext_node(cinfo->cache,
                                               requests->requestvb->name,
                                               requests->requestvb->name_length);
            if (cnode && cnode->thedata) {
                cdata = cnode->thedata;
                if (cdata->val.string && cdata->name && cdata->name_length) {
                    DEBUGMSGTL(("helper:stash_cache", "Found cached GETNEXT varbind\n"));
                    DEBUGMSGOID(("helper:stash_cache", cdata->name, cdata->name_length));
                    DEBUGMSG(("helper:stash_cache", "\n"));
                    snmp_set_var_typed_value(request->requestvb, cdata->type,
                                             cdata->val.string, cdata->val_len);
                    snmp_set_var_objid(request->requestvb, cdata->name,
                                       cdata->name_length);
                }
            }
        }
        return SNMP_ERR_NOERROR;
        break;

    default:
        cinfo->cache_valid = 0;
        return netsnmp_call_next_handler(handler, reginfo, reqinfo,
                                         requests);
    }
    return SNMP_ERR_GENERR;     /* should never get here */
}

/** updates a given cache depending on whether it needs to or not.
 */
int
_netsnmp_stash_cache_load( netsnmp_cache *cache, void *magic )
{
    netsnmp_mib_handler          *handler  = cache->handler;
    netsnmp_handler_registration *reginfo  = cache->reginfo;
    netsnmp_agent_request_info   *reqinfo  = cache->reqinfo;
    netsnmp_request_info         *requests = cache->requests;
    netsnmp_stash_cache_info     *cinfo    = (netsnmp_stash_cache_info*) magic;
    int old_mode;
    int ret;

    if (!cinfo) {
        cinfo = netsnmp_get_new_stash_cache();
        cache->magic = cinfo;
    }

    /* change modes to the GET_STASH mode */
    old_mode = reqinfo->mode;
    reqinfo->mode = MODE_GET_STASH;
    netsnmp_agent_add_list_data(reqinfo,
                                netsnmp_create_data_list(STASH_CACHE_NAME,
                                                         &cinfo->cache, NULL));

    /* have the next handler fill stuff in and switch modes back */
    ret = netsnmp_call_next_handler(handler->next, reginfo, reqinfo, requests);
    reqinfo->mode = old_mode;

    return ret;
}

void
_netsnmp_stash_cache_free( netsnmp_cache *cache, void *magic )
{
    netsnmp_stash_cache_info *cinfo = (netsnmp_stash_cache_info*) magic;
    netsnmp_oid_stash_free(&cinfo->cache,
                          (NetSNMPStashFreeNode *) snmp_free_var);
    return;
}

/** initializes the stash_cache helper which then registers a stash_cache
 *  handler as a run-time injectable handler for configuration file
 *  use.
 */
void
netsnmp_init_stash_cache_helper(void)
{
    netsnmp_register_handler_by_name("stash_cache",
                                     netsnmp_get_stash_cache_handler());
}
