/*
 * Copyright (c) 2014-2016, Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2017-2018 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <netdb.h>
#include <inttypes.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>

#include "ofi_prov.h"
#include <ofi_util.h>

#include "efa.h"
#include "efa_ib.h"
#include "efa_io_defs.h"
#include "efa_verbs.h"

#define EFA_FABRIC_PREFIX "EFA-"

#define EFA_DOMAIN_CAPS (FI_LOCAL_COMM | FI_REMOTE_COMM)

#define EFA_RDM_CAPS (FI_MSG | FI_RECV | FI_SEND | FI_SOURCE | EFA_DOMAIN_CAPS)
#define EFA_DGRM_CAPS (FI_MSG | FI_RECV | FI_SEND | FI_SOURCE | EFA_DOMAIN_CAPS)

#define EFA_TX_OP_FLAGS (FI_TRANSMIT_COMPLETE)

#define EFA_RX_MODE (0)

#define EFA_RX_RDM_OP_FLAGS (0)
#define EFA_RX_DGRM_OP_FLAGS (0)

#define EFA_MSG_ORDER (FI_ORDER_NONE)

#define EFA_NO_DEFAULT -1

#define EFA_DEF_MR_CACHE_ENABLE 1
#define EFA_DEF_MR_CACHE_MERGE_REGIONS 1

int efa_mr_cache_enable		= EFA_DEF_MR_CACHE_ENABLE;
int efa_mr_cache_merge_regions	= EFA_DEF_MR_CACHE_MERGE_REGIONS;
size_t efa_mr_max_cached_count;
size_t efa_mr_max_cached_size;

static void efa_addr_to_str(const uint8_t *raw_addr, char *str);
static int efa_get_addr(struct efa_context *ctx, void *src_addr);

const struct fi_fabric_attr efa_fabric_attr = {
	.fabric		= NULL,
	.name		= NULL,
	.prov_name	= NULL,
	.prov_version	= EFA_PROV_VERS,
};

const struct fi_domain_attr efa_domain_attr = {
	.caps			= EFA_DOMAIN_CAPS,
	.threading		= FI_THREAD_DOMAIN,
	.control_progress	= FI_PROGRESS_AUTO,
	.data_progress		= FI_PROGRESS_AUTO,
	.resource_mgmt		= FI_RM_DISABLED,

	.mr_mode		= OFI_MR_BASIC_MAP | FI_MR_LOCAL | FI_MR_BASIC,
	.mr_key_size		= sizeof_field(struct efa_io_tx_buf_desc, lkey),
	.cq_data_size		= 0,
	.tx_ctx_cnt		= 1024,
	.rx_ctx_cnt		= 1024,
	.max_ep_tx_ctx		= 1,
	.max_ep_rx_ctx		= 1,
	.mr_iov_limit		= EFA_MR_IOV_LIMIT,
};

const struct fi_ep_attr efa_ep_attr = {
	.protocol		= FI_PROTO_EFA,
	.protocol_version	= 1,
	.msg_prefix_size	= 0,
	.max_order_war_size	= 0,
	.mem_tag_format		= 0,
	.tx_ctx_cnt		= 1,
	.rx_ctx_cnt		= 1,
};

const struct fi_rx_attr efa_dgrm_rx_attr = {
	.mode			= FI_MSG_PREFIX | EFA_RX_MODE,
	.op_flags		= EFA_RX_DGRM_OP_FLAGS,
	.msg_order		= EFA_MSG_ORDER,
	.comp_order		= FI_ORDER_NONE,
	.total_buffered_recv	= 0,
	.iov_limit		= 1
};

const struct fi_rx_attr efa_rdm_rx_attr = {
	.mode			= EFA_RX_MODE,
	.op_flags		= EFA_RX_RDM_OP_FLAGS,
	.msg_order		= EFA_MSG_ORDER,
	.comp_order		= FI_ORDER_NONE,
	.total_buffered_recv	= 0,
	.iov_limit		= 1
};

const struct fi_tx_attr efa_dgrm_tx_attr = {
	.mode			= FI_MSG_PREFIX,
	.op_flags		= EFA_TX_OP_FLAGS,
	.msg_order		= EFA_MSG_ORDER,
	.comp_order		= FI_ORDER_NONE,
	.inject_size		= 0,
	.rma_iov_limit		= 0,
};

const struct fi_tx_attr efa_rdm_tx_attr = {
	.mode			= 0,
	.op_flags		= EFA_TX_OP_FLAGS,
	.msg_order		= EFA_MSG_ORDER,
	.comp_order		= FI_ORDER_NONE,
	.inject_size		= 0,
	.rma_iov_limit		= 0,
};

const struct efa_ep_domain efa_rdm_domain = {
	.suffix			= "-rdm",
	.type			= FI_EP_RDM,
	.caps			= EFA_RDM_CAPS,
};

const struct efa_ep_domain efa_dgrm_domain = {
	.suffix			= "-dgrm",
	.type			= FI_EP_DGRAM,
	.caps			= EFA_DGRM_CAPS,
};

static int efa_check_hints(uint32_t version, const struct fi_info *hints,
			   const struct fi_info *info)
{
	uint64_t prov_mode;
	size_t size;
	int ret;

	if (hints->caps & ~(info->caps)) {
		EFA_INFO(FI_LOG_CORE, "Unsupported capabilities\n");
		FI_INFO_CHECK(&efa_prov, info, hints, caps, FI_TYPE_CAPS);
		return -FI_ENODATA;
	}

	prov_mode = ofi_mr_get_prov_mode(version, hints, info);

	if ((hints->mode & prov_mode) != prov_mode) {
		EFA_INFO(FI_LOG_CORE, "Required hints mode bits not set\n");
		FI_INFO_MODE(&efa_prov, prov_mode, hints->mode);
		return -FI_ENODATA;
	}

	if (hints->fabric_attr) {
		ret = ofi_check_fabric_attr(&efa_prov, info->fabric_attr,
					    hints->fabric_attr);

		if (ret)
			return ret;
	}

	switch (hints->addr_format) {
	case FI_FORMAT_UNSPEC:
	case FI_ADDR_EFA:
		size = EFA_EP_ADDR_LEN;
		break;
	default:
		EFA_INFO(FI_LOG_CORE,
			 "Address format not supported: hints[%u], supported[%u,%u]\n",
			 hints->addr_format, FI_FORMAT_UNSPEC, FI_ADDR_EFA);
		return -FI_ENODATA;
	}

	if (hints->src_addr && hints->src_addrlen < size)
		return -FI_ENODATA;

	if (hints->dest_addr && hints->dest_addrlen < size)
		return -FI_ENODATA;

	if (hints->domain_attr) {
		ret = ofi_check_domain_attr(&efa_prov, version, info->domain_attr, hints);
		if (ret)
			return ret;
	}

	if (hints->ep_attr) {
		ret = ofi_check_ep_attr(&efa_util_prov, info->fabric_attr->api_version, info, hints);
		if (ret)
			return ret;
	}

	if (hints->rx_attr) {
		ret = ofi_check_rx_attr(&efa_prov, info, hints->rx_attr, hints->mode);
		if (ret)
			return ret;
	}

	if (hints->tx_attr) {
		ret = ofi_check_tx_attr(&efa_prov, info->tx_attr, hints->tx_attr, hints->mode);
		if (ret)
			return ret;
	}

	return 0;
}

static int efa_alloc_qp_table(struct efa_context *ctx, size_t ep_cnt)
{
	ctx->qp_table = calloc(ep_cnt, sizeof(*ctx->qp_table));
	if (!ctx->qp_table)
		return -FI_ENOMEM;
	pthread_mutex_init(&ctx->qp_table_mutex, NULL);

	return FI_SUCCESS;
}

static void efa_free_qp_table(struct efa_context *ctx)
{
	pthread_mutex_destroy(&ctx->qp_table_mutex);
	free(ctx->qp_table);
}

static int efa_alloc_fid_nic(struct fi_info *fi, struct efa_context *ctx,
			     struct efa_device_attr *efa_device_attr,
			     struct ibv_port_attr *port_attr)
{
	struct fi_device_attr *device_attr;
	char driver_real_path[PATH_MAX];
	struct fi_link_attr *link_attr;
	char dbdf_real_path[PATH_MAX];
	struct fi_bus_attr *bus_attr;
	struct fi_pci_attr *pci_attr;
	char *driver_sym_path;
	char *dbdf_sym_path;
	char *sysfs_path;
	void *src_addr;
	char *driver;
	int name_len;
	char *dbdf;
	int ret;

	/* Sets nic ops and allocates basic structure */
	fi->nic = ofi_nic_dup(NULL);
	if (!fi->nic)
		return -FI_ENOMEM;

	device_attr = fi->nic->device_attr;
	bus_attr = fi->nic->bus_attr;
	pci_attr = &bus_attr->attr.pci;
	link_attr = fi->nic->link_attr;

	/* fi_device_attr */
	device_attr->name = strdup(ctx->ibv_ctx.device->name);
	if (!device_attr->name) {
		ret = -FI_ENOMEM;
		goto err_free_nic;
	}

	ret = asprintf(&device_attr->device_id, "0x%x",
		       efa_device_attr->ibv_attr.vendor_part_id);
	/* ofi_nic_close will free all attributes of the fi_nic struct */
	if (ret < 0) {
		ret = -FI_ENOMEM;
		goto err_free_nic;
	}

	device_attr->device_version = calloc(1, EFA_ABI_VER_MAX_LEN + 1);
	if (!device_attr->device_version) {
		ret = -FI_ENOMEM;
		goto err_free_nic;
	}

	sysfs_path = get_sysfs_path();
	if (!sysfs_path) {
		ret = -FI_ENOMEM;
		goto err_free_nic;
	}

	ret = fi_read_file(sysfs_path, "class/infiniband_verbs/abi_version",
			   device_attr->device_version,
			   sizeof(device_attr->device_version));
	if (ret < 0)
		goto err_free_sysfs;

	ret = asprintf(&device_attr->vendor_id, "0x%x",
		       efa_device_attr->ibv_attr.vendor_id);
	if (ret < 0) {
		ret = -FI_ENOMEM;
		goto err_free_sysfs;
	}

	ret = asprintf(&driver_sym_path, "%s%s",
		       ctx->ibv_ctx.device->ibdev_path, "/device/driver");
	if (ret < 0) {
		ret = -FI_ENOMEM;
		goto err_free_sysfs;
	}

	if (!realpath(driver_sym_path, driver_real_path)) {
		ret = -errno;
		goto err_free_driver_sym;
	}

	driver = strrchr(driver_real_path, '/');
	if (!driver) {
		ret = -FI_EINVAL;
		goto err_free_driver_sym;
	}
	driver++;
	device_attr->driver = strdup(driver);
	if (!device_attr->driver) {
		ret = -FI_ENOMEM;
		goto err_free_driver_sym;
	}

	device_attr->firmware = strdup(efa_device_attr->ibv_attr.fw_ver);
	if (!device_attr->firmware) {
		ret = -FI_ENOMEM;
		goto err_free_driver_sym;
	}

	/* fi_bus_attr */
	bus_attr->bus_type = FI_BUS_PCI;

	/* fi_pci_attr */
	ret = asprintf(&dbdf_sym_path, "%s%s",
		       ctx->ibv_ctx.device->ibdev_path, "/device");
	if (ret < 0) {
		ret = -FI_ENOMEM;
		goto err_free_driver_sym;
	}

	if (!realpath(dbdf_sym_path, dbdf_real_path)) {
		ret = -errno;
		goto err_free_dbdf_sym;
	}

	dbdf = strrchr(dbdf_real_path, '/');
	if (!dbdf) {
		ret = -FI_EINVAL;
		goto err_free_dbdf_sym;
	}
	dbdf++;

	ret = sscanf(dbdf, "%hx:%hhx:%hhx.%hhx", &pci_attr->domain_id,
		     &pci_attr->bus_id, &pci_attr->device_id,
		     &pci_attr->function_id);
	if (ret != 4) {
		ret = -FI_EINVAL;
		goto err_free_dbdf_sym;
	}

	/* fi_link_attr */
	src_addr = calloc(1, EFA_EP_ADDR_LEN);
	if (!src_addr) {
		ret = -FI_ENOMEM;
		goto err_free_dbdf_sym;
	}

	ret = efa_get_addr(ctx, src_addr);
	if (ret)
		goto err_free_src_addr;

	name_len = strlen(EFA_FABRIC_PREFIX) + INET6_ADDRSTRLEN;
	link_attr->address = calloc(1, name_len + 1);
	if (!link_attr->address) {
		ret = -FI_ENOMEM;
		goto err_free_src_addr;
	}

	efa_addr_to_str(src_addr, link_attr->address);

	link_attr->mtu = port_attr->max_msg_sz;

	link_attr->speed = 0;

	switch (port_attr->state) {
	case IBV_PORT_DOWN:
		link_attr->state = FI_LINK_DOWN;
		break;
	case IBV_PORT_ACTIVE:
		link_attr->state = FI_LINK_UP;
		break;
	default:
		link_attr->state = FI_LINK_UNKNOWN;
		break;
	}

	link_attr->network_type = strdup("Ethernet");
	if (!link_attr->network_type) {
		ret = -FI_ENOMEM;
		goto err_free_src_addr;
	}

	free(src_addr);
	free(dbdf_sym_path);
	free(driver_sym_path);
	free(sysfs_path);
	return FI_SUCCESS;

err_free_src_addr:
	free(src_addr);
err_free_dbdf_sym:
	free(dbdf_sym_path);
err_free_driver_sym:
	free(driver_sym_path);
err_free_sysfs:
	free(sysfs_path);
err_free_nic:
	fi_close(&fi->nic->fid);
	fi->nic = NULL;
	return ret;
}

static int efa_get_device_attrs(struct efa_context *ctx, struct fi_info *info)
{
	struct efa_device_attr device_attr;
	struct ibv_device_attr *base_attr;
	struct ibv_port_attr port_attr;
	int ret;

	base_attr = &device_attr.ibv_attr;
	ret = efa_cmd_query_device(ctx, &device_attr);
	if (ret) {
		EFA_INFO_ERRNO(FI_LOG_FABRIC, "efa_verbs_query_device_ex", ret);
		return ret;
	}

	ctx->max_mr_size			= base_attr->max_mr_size;
	info->domain_attr->cq_cnt		= base_attr->max_cq;
	info->domain_attr->ep_cnt		= base_attr->max_qp;
	info->domain_attr->tx_ctx_cnt		= MIN(info->domain_attr->tx_ctx_cnt, base_attr->max_qp);
	info->domain_attr->rx_ctx_cnt		= MIN(info->domain_attr->rx_ctx_cnt, base_attr->max_qp);
	info->domain_attr->max_ep_tx_ctx	= 1;
	info->domain_attr->max_ep_rx_ctx	= 1;
	info->domain_attr->resource_mgmt	= FI_RM_DISABLED;
	info->domain_attr->mr_cnt		= base_attr->max_mr;

	EFA_DBG(FI_LOG_DOMAIN, "Domain attribute :\n"
				"\t info->domain_attr->cq_cnt		= %zu\n"
				"\t info->domain_attr->ep_cnt		= %zu\n"
				"\t info->domain_attr->rx_ctx_cnt	= %zu\n"
				"\t info->domain_attr->tx_ctx_cnt	= %zu\n"
				"\t info->domain_attr->max_ep_tx_ctx	= %zu\n"
				"\t info->domain_attr->max_ep_rx_ctx	= %zu\n",
				info->domain_attr->cq_cnt,
				info->domain_attr->ep_cnt,
				info->domain_attr->tx_ctx_cnt,
				info->domain_attr->rx_ctx_cnt,
				info->domain_attr->max_ep_tx_ctx,
				info->domain_attr->max_ep_rx_ctx);

	info->tx_attr->iov_limit	= device_attr.max_sq_sge;
	info->tx_attr->size		= align_down_to_power_of_2(MIN(device_attr.max_sq_wr,
								   ctx->max_llq_size / sizeof(struct efa_io_tx_wqe)));
	info->rx_attr->iov_limit	= device_attr.max_rq_sge;
	info->rx_attr->size		= align_down_to_power_of_2(device_attr.max_rq_wr / info->rx_attr->iov_limit);

	EFA_DBG(FI_LOG_DOMAIN, "Tx/Rx attribute :\n"
				"\t info->tx_attr->iov_limit		= %zu\n"
				"\t info->tx_attr->size			= %zu\n"
				"\t info->rx_attr->iov_limit		= %zu\n"
				"\t info->rx_attr->size			= %zu\n",
				info->tx_attr->iov_limit,
				info->tx_attr->size,
				info->rx_attr->iov_limit,
				info->rx_attr->size);

	ret = efa_cmd_query_port(ctx, 1, &port_attr);
	if (ret) {
		EFA_INFO_ERRNO(FI_LOG_FABRIC, "ibv_query_port", errno);
		return ret;
	}

	info->ep_attr->max_msg_size		= port_attr.max_msg_sz;
	info->ep_attr->max_order_raw_size	= port_attr.max_msg_sz;
	info->ep_attr->max_order_waw_size	= port_attr.max_msg_sz;

	EFA_DBG(FI_LOG_DOMAIN, "Internal attributes:\n"
				"\tinject size        = %" PRIu16 "\n"
				"\tsub_cqs_per_cq     = %" PRIu16 "\n",
				ctx->inject_size, ctx->sub_cqs_per_cq);

	/* Set fid nic attributes. */
	ret = efa_alloc_fid_nic(info, ctx, &device_attr, &port_attr);
	if (ret) {
		EFA_WARN(FI_LOG_FABRIC,
			 "Unable to allocate fid_nic: %s\n", fi_strerror(-ret));
		return ret;
	}

	return 0;
}

static void efa_addr_to_str(const uint8_t *raw_addr, char *str)
{
	size_t name_len = strlen(EFA_FABRIC_PREFIX) + INET6_ADDRSTRLEN;
	char straddr[INET6_ADDRSTRLEN] = {};

	if (!inet_ntop(AF_INET6, raw_addr, straddr, INET6_ADDRSTRLEN))
		return;
	snprintf(str, name_len, EFA_FABRIC_PREFIX "%s", straddr);
}

static int efa_str_to_ep_addr(const char *node, const char *service, struct efa_ep_addr *addr)
{
	int ret;

	if (!node)
		return -FI_EINVAL;

	memset(addr, 0, sizeof(*addr));

	ret = inet_pton(AF_INET6, node, addr->raw);
	if (ret != 1)
		return -FI_EINVAL;
	if (service)
		addr->qpn = atoi(service);

	return 0;
}

static int efa_get_addr(struct efa_context *ctx, void *src_addr)
{
	union ibv_gid gid;
	int ret;

	ret = efa_cmd_query_gid(ctx, 1, 0, &gid);
	if (ret) {
		EFA_INFO_ERRNO(FI_LOG_FABRIC, "efa_cmd_query_gid", errno);
		return ret;
	}

	memcpy(src_addr, &gid, sizeof(gid));

	return 0;
}

static int efa_alloc_info(struct efa_context *ctx, struct fi_info **info,
			  const struct efa_ep_domain *ep_dom)
{
	struct fi_info *fi;
	union ibv_gid gid;
	size_t name_len;
	int ret;

	fi = fi_allocinfo();
	if (!fi)
		return -FI_ENOMEM;

	fi->caps		= ep_dom->caps;
	fi->handle		= NULL;
	*fi->ep_attr		= efa_ep_attr;
	if (ep_dom->type == FI_EP_RDM) {
		*fi->tx_attr	= efa_rdm_tx_attr;
		*fi->rx_attr	= efa_rdm_rx_attr;
	} else if (ep_dom->type == FI_EP_DGRAM) {
		fi->mode |= FI_MSG_PREFIX;
		fi->ep_attr->msg_prefix_size = 40;
		*fi->tx_attr	= efa_dgrm_tx_attr;
		*fi->rx_attr	= efa_dgrm_rx_attr;
	}

	*fi->domain_attr	= efa_domain_attr;
	*fi->fabric_attr	= efa_fabric_attr;

	fi->ep_attr->protocol	= FI_PROTO_EFA;
	fi->ep_attr->type	= ep_dom->type;
	fi->tx_attr->caps	= ep_dom->caps;
	fi->rx_attr->caps	= ep_dom->caps;

	ret = efa_get_device_attrs(ctx, fi);
	if (ret)
		goto err_free_info;

	ret = efa_cmd_query_gid(ctx, 1, 0, &gid);
	if (ret) {
		EFA_INFO_ERRNO(FI_LOG_FABRIC, "efa_cmd_query_gid", errno);
		ret = -errno;
		goto err_free_info;
	}

	name_len = strlen(EFA_FABRIC_PREFIX) + INET6_ADDRSTRLEN;

	fi->fabric_attr->name = calloc(1, name_len + 1);
	if (!fi->fabric_attr->name) {
		ret = -FI_ENOMEM;
		goto err_free_info;
	}
	efa_addr_to_str(gid.raw, fi->fabric_attr->name);

	name_len = strlen(ctx->ibv_ctx.device->name) + strlen(ep_dom->suffix);
	fi->domain_attr->name = malloc(name_len + 1);
	if (!fi->domain_attr->name) {
		ret = -FI_ENOMEM;
		goto err_free_fab_name;
	}

	snprintf(fi->domain_attr->name, name_len + 1, "%s%s",
		 ctx->ibv_ctx.device->name, ep_dom->suffix);
	fi->domain_attr->name[name_len] = '\0';

	fi->addr_format = FI_ADDR_EFA;
	fi->src_addr = calloc(1, EFA_EP_ADDR_LEN);
	if (!fi->src_addr) {
		ret = -FI_ENOMEM;
		goto err_free_dom_name;
	}
	fi->src_addrlen = EFA_EP_ADDR_LEN;
	ret = efa_get_addr(ctx, fi->src_addr);
	if (ret)
		goto err_free_src;

	fi->domain_attr->av_type = FI_AV_TABLE;

	*info = fi;
	return 0;

err_free_src:
	free(fi->src_addr);
err_free_dom_name:
	free(fi->domain_attr->name);
err_free_fab_name:
	free(fi->fabric_attr->name);
err_free_info:
	fi_freeinfo(fi);
	return ret;
}

const struct fi_info *efa_get_efa_info(const char *domain_name)
{
	const struct fi_info *fi;

	for (fi = efa_util_prov.info; fi; fi = fi->next) {
		if (!strcmp(fi->domain_attr->name, domain_name))
			return fi;
	}

	return NULL;
}

static int efa_node_matches_addr(struct efa_ep_addr *addr, const char *node)
{
	struct efa_ep_addr eaddr;

	efa_str_to_ep_addr(node, NULL, &eaddr);
	return memcmp(&eaddr.raw, &addr->raw, sizeof(addr->raw));
}

static int efa_get_matching_info(uint32_t version, const char *node, uint64_t flags,
				 const struct fi_info *hints, struct fi_info **info)
{
	const struct fi_info *check_info;
	struct fi_info *fi, *tail;
	int ret;

	*info = tail = NULL;

	for (check_info = efa_util_prov.info; check_info; check_info = check_info->next) {
		ret = 0;
		if (flags & FI_SOURCE) {
			if (node)
				ret = efa_node_matches_addr(check_info->src_addr, node);
		} else if (hints && hints->src_addr) {
			ret = memcmp(check_info->src_addr, hints->src_addr, EFA_EP_ADDR_LEN);
		}

		if (ret)
			continue;
		EFA_INFO(FI_LOG_FABRIC, "found match for interface %s %s\n", node, check_info->fabric_attr->name);
		if (hints) {
			ret = efa_check_hints(version, hints, check_info);
			if (ret)
				continue;
		}

		fi = fi_dupinfo(check_info);
		if (!fi) {
			ret = -FI_ENOMEM;
			goto err_free_info;
		}

		fi->fabric_attr->api_version = version;

		if (!*info)
			*info = fi;
		else
			tail->next = fi;
		tail = fi;
	}

	if (!*info)
		return -FI_ENODATA;

	return 0;

err_free_info:
	fi_freeinfo(*info);
	*info = NULL;
	return ret;
}

static int efa_set_fi_address(const char *node, const char *service, uint64_t flags,
			      const struct fi_info *hints, struct fi_info *fi)
{
	struct efa_ep_addr tmp_addr;
	void *dest_addr = NULL;
	int ret = FI_SUCCESS;

	if (flags & FI_SOURCE) {
		if (hints && hints->dest_addr)
			dest_addr = hints->dest_addr;
	} else {
		if (node || service) {
			ret = efa_str_to_ep_addr(node, service, &tmp_addr);
			if (ret)
				return ret;
			dest_addr = &tmp_addr;
		} else if (hints && hints->dest_addr) {
			dest_addr = hints->dest_addr;
		}
	}

	if (dest_addr) {
		fi->dest_addr = malloc(EFA_EP_ADDR_LEN);
		if (!fi->dest_addr)
			return -FI_ENOMEM;
		memcpy(fi->dest_addr, dest_addr, EFA_EP_ADDR_LEN);
	}

	if (fi->dest_addr)
		fi->dest_addrlen = EFA_EP_ADDR_LEN;

	return ret;
}

int efa_getinfo(uint32_t version, const char *node, const char *service,
		uint64_t flags, const struct fi_info *hints, struct fi_info **info)
{
	int ret;

	if (!(flags & FI_SOURCE) && hints && hints->src_addr &&
	    hints->src_addrlen != EFA_EP_ADDR_LEN)
		return -FI_ENODATA;

	if (((!node && !service) || (flags & FI_SOURCE)) &&
	    hints && hints->dest_addr &&
	    hints->dest_addrlen != EFA_EP_ADDR_LEN)
		return -FI_ENODATA;

	ret = efa_get_matching_info(version, node, flags, hints, info);
	if (ret)
		goto out;

	ret = efa_set_fi_address(node, service, flags, hints, *info);
	if (ret)
		goto out;

	ofi_alter_info(*info, hints, version);

out:
	if (!ret || ret == -FI_ENOMEM || ret == -FI_ENODEV) {
		return ret;
	} else {
		fi_freeinfo(*info);
		*info = NULL;
		return -FI_ENODATA;
	}
}

static int efa_fabric_close(fid_t fid)
{
	struct efa_fabric *fab;
	int ret;

	fab = container_of(fid, struct efa_fabric, util_fabric.fabric_fid.fid);
	ret = ofi_fabric_close(&fab->util_fabric);
	if (ret)
		return ret;
	free(fab);

	return 0;
}

static struct fi_ops efa_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = efa_fabric_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static struct fi_ops_fabric efa_ops_fabric = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = efa_domain_open,
	.passive_ep = fi_no_passive_ep,
	.eq_open = ofi_eq_create,
	.wait_open = ofi_wait_fd_open,
	.trywait = ofi_trywait
};

int efa_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric_fid,
	       void *context)
{
	const struct fi_info *info;
	struct efa_fabric *fab;
	int ret = 0;

	fab = calloc(1, sizeof(*fab));
	if (!fab)
		return -FI_ENOMEM;

	for (info = efa_util_prov.info; info; info = info->next) {
		ret = ofi_fabric_init(&efa_prov, info->fabric_attr, attr,
				      &fab->util_fabric, context);
		if (ret != -FI_ENODATA)
			break;
	}
	if (ret) {
		free(fab);
		return ret;
	}

	*fabric_fid = &fab->util_fabric.fabric_fid;
	(*fabric_fid)->fid.fclass = FI_CLASS_FABRIC;
	(*fabric_fid)->fid.ops = &efa_fi_ops;
	(*fabric_fid)->ops = &efa_ops_fabric;
	(*fabric_fid)->api_version = attr->api_version;

	return 0;
}

static void efa_dealloc_ctx(struct efa_context *ctx)
{
	efa_free_qp_table(ctx);
}

static void fi_efa_fini(void)
{
	struct efa_context **ctx_list;
	int num_devices;
	int i;

	fi_freeinfo((void *)efa_util_prov.info);
	efa_util_prov.info = NULL;

	ctx_list = efa_device_get_context_list(&num_devices);
	for (i = 0; i < num_devices; i++)
		efa_dealloc_ctx(ctx_list[i]);
	efa_device_free_context_list(ctx_list);
	efa_device_free();
}

struct fi_provider efa_prov = {
	.name = EFA_PROV_NAME,
	.version = EFA_PROV_VERS,
	.fi_version = FI_VERSION(1, 8),
	.getinfo = efa_getinfo,
	.fabric = efa_fabric,
	.cleanup = fi_efa_fini
};

struct util_prov efa_util_prov = {
	.prov = &efa_prov,
	.info = NULL,
	.flags = 0,
};

static int efa_init_info(const struct fi_info **all_infos)
{
	struct efa_context **ctx_list;
	int ret, retv = 1, i, num_devices;
	struct fi_info *tail = NULL;
	struct fi_info *fi = NULL;

	ret = efa_device_init();
	if (ret)
		return ret;

	ctx_list = efa_device_get_context_list(&num_devices);
	if (!num_devices)
		return -FI_ENODEV;

	*all_infos = NULL;
	for (i = 0; i < num_devices; i++) {
		ret = efa_alloc_info(ctx_list[i], &fi, &efa_rdm_domain);
		if (!ret) {
			if (!*all_infos)
				*all_infos = fi;
			else
				tail->next = fi;
			tail = fi;
			ret = efa_alloc_info(ctx_list[i], &fi, &efa_dgrm_domain);
			if (!ret) {
				tail->next = fi;
				tail = fi;
			}
		} else {
			continue;
		}

		ret = efa_alloc_qp_table(ctx_list[i], tail->domain_attr->ep_cnt);
		if (!ret)
			retv = 0;
	}

	efa_device_free_context_list(ctx_list);
	if (retv)
		return ret;
	return retv;
}

struct fi_provider *init_lower_efa_prov()
{
	if (efa_init_info(&efa_util_prov.info))
		return NULL;

	return &efa_prov;
}
