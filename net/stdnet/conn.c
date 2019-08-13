#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_core.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_utils.h"
#include "3part.h"

static int __conn_add(const nid_t *nid)
{
        int ret;
        char key[MAX_NAME_LEN], buf[MAX_BUF_LEN], tmp[MAX_BUF_LEN];
        ltg_net_info_t *info;
        net_handle_t nh;
        size_t len;

        if (netable_connected(nid)) {
                netable_update(nid);
                goto out;
        }

        snprintf(key, MAX_NAME_LEN, "%u.info", nid->id);

        ret = etcd_get_text(ETCD_CONN, key, tmp, NULL);
        if (ret) {
                goto out;
        }

        len = MAX_BUF_LEN;
        info = (void *)buf;
        ret = urlsafe_b64_decode(tmp, strlen(tmp), (void *)info, &len);
        LTG_ASSERT(ret == 0);        

        DINFO("connect to %u %s\n", nid->id, info->name);

        ret = netable_connect_info(&nh, info, 1);
        if (ret) {
                DINFO("connect to %u %s fail\n", nid->id, info->name);
                GOTO(err_ret, ret);
        }

out:

        return 0;
err_ret:
        return ret;
}

static int __conn_scan__()
{
        int ret, i;
        etcd_node_t *list = NULL, *node;
        nid_t nid;

        ret = etcd_list(ETCD_CONN, &list);
        if (unlikely(ret)) {
                if (ret == ENOKEY) {
                        DINFO("conn table empty\n");
                        goto out;
                } else
                        GOTO(err_ret, ret);
        }

        for(i = 0; i < list->num_node; i++) {
                node = list->nodes[i];
 
                if (strstr(node->key, ".info") == NULL) {
                        DBUG("skip %s\n", node->key);
                        continue;
                }

                str2nid(&nid, node->key);
                ret = __conn_add(&nid);
        }

        free_etcd_node(list);

out:
        return 0;
err_ret:
        return ret;
}

static void *__conn_scan(void *arg)
{
        (void) arg;
        
        while (1) {
                sleep(ltgconf.rpc_timeout / 2);
                __conn_scan__();
        }

        pthread_exit(NULL);
}

int conn_init()
{
        int ret;

        ret = ltg_thread_create(__conn_scan, NULL, "__conn_scan");
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

static void *__conn_retry(void *arg)
{
        int retry = 0;
        const nid_t *nid = arg;

        while (1) {
                if (retry > ltgconf.rpc_timeout * 2 && !conn_online(nid, -1)) {
                        DINFO("retry conn to %s fail, exit\n", network_rname(nid));
                        break;
                }
                
                __conn_add(nid);
                if (netable_connected(nid)) {
                        DINFO("retry conn to %s success\n", network_rname(nid));
                        break;
                }

                DINFO("retry conn to %s, sleep %u\n", network_rname(nid), retry);
                retry++;

                sleep(1);
        }

        ltg_free((void **)&arg);
        pthread_exit(NULL);
}

int conn_retry(const nid_t *_nid)
{
        int ret;
        nid_t *nid;

        ret = ltg_malloc((void**)&nid, sizeof(*nid));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        *nid = *_nid;

        ret = ltg_thread_create(__conn_retry, nid, "__conn_scan");
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __conn_init_info(nid_t *_nid)
{
        int ret, retry = 0;
        char key[MAX_NAME_LEN], buf[MAX_BUF_LEN], tmp[MAX_BUF_LEN];
        ltg_net_info_t *info;
        uint32_t buflen;
        size_t size;
        nid_t nid;

        info = (void *)buf;
        buflen = MAX_BUF_LEN;
        ret = rpc_getinfo(buf, &buflen);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        LTG_ASSERT(info->len);
        LTG_ASSERT(info->info_count);
        size = MAX_BUF_LEN;
        ret = urlsafe_b64_encode((void *)info, info->len, tmp, &size);
        LTG_ASSERT(ret == 0);

        nid = *net_getnid();
        LTG_ASSERT(nid.id == info->id.id);
        snprintf(key, MAX_NAME_LEN, "%u.info", nid.id);

retry:
        DBUG("register %s value %s\n", key, tmp);
        ret = etcd_create_text(ETCD_CONN, key, tmp, 0);
        if (unlikely(ret)) {
                ret = etcd_update_text(ETCD_CONN, key, tmp, NULL, 0);
                if (unlikely(ret)) {
                        USLEEP_RETRY(err_ret, ret, retry, retry, 30, (1000 * 1000));
                }
        }

        *_nid = nid;

        return 0;
err_ret:
        return ret;
}

int conn_register()
{
        int ret;
        nid_t nid;

        ret = __conn_init_info(&nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int conn_getinfo(const nid_t *nid, ltg_net_info_t *info)
{
        int ret;
        char key[MAX_NAME_LEN], tmp[MAX_BUF_LEN];
        size_t  len;

        snprintf(key, MAX_NAME_LEN, "%u.info", nid->id);
        ret = etcd_get_text(ETCD_CONN, key, tmp, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("get %s value %s\n", key, tmp);
        len = MAX_BUF_LEN;
        ret = urlsafe_b64_decode(tmp, strlen(tmp), (void *)info, &len);
        LTG_ASSERT(ret == 0);
        LTG_ASSERT(info->info_count * sizeof(sock_info_t) + sizeof(ltg_net_info_t) == info->len);
        LTG_ASSERT(info->info_count);

        return 0;
err_ret:
        return ret;
}


int conn_setinfo()
{
        int ret;
        char key[MAX_NAME_LEN], buf[MAX_BUF_LEN], tmp[MAX_BUF_LEN];
        ltg_net_info_t *info;
        uint32_t buflen;
        size_t size;

        info = (void *)buf;
        buflen = MAX_BUF_LEN;
        ret = rpc_getinfo(buf, &buflen);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        LTG_ASSERT(info->len);
        LTG_ASSERT(info->info_count);
        size = MAX_BUF_LEN;
        ret = urlsafe_b64_encode((void *)info, info->len, tmp, &size);
        LTG_ASSERT(ret == 0);
        
        snprintf(key, MAX_NAME_LEN, "%u.info", info->id.id);

        DINFO("register %s value %s\n", key, tmp);
        ret = etcd_create_text(ETCD_CONN, key, tmp, 0);
        if (unlikely(ret)) {
                ret = etcd_update_text(ETCD_CONN, key, tmp, NULL, 0);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }
        
        return 0;
err_ret:
        return ret;
}

int conn_online(const nid_t *nid, int _tmo)
{
        int tmo;

        DBUG("conn_online not implimented\n");

        if (netable_connected(nid))
                return 1;

        tmo = _tmo == -1 ? ltgconf.rpc_timeout : _tmo;
        time_t last_update = netable_last_update(nid);

        if (gettime() - last_update < tmo) {
                return 1;
        }

        return 0;
}