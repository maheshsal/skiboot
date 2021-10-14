/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 * Copyright 2021 IBM Corp.
 */

#ifndef __PAU_H
#define __PAU_H

#include <io.h>
#include <pci.h>
#include <xscom.h>
#include <phb4.h>
#include <pau-regs.h>

#define PAU_NBR 6
#define PAU_LINKS_OPENCAPI_PER_PAU 2

enum pau_dev_type {
	PAU_DEV_TYPE_UNKNOWN = 0,
	PAU_DEV_TYPE_OPENCAPI,
	PAU_DEV_TYPE_ANY = INT_MAX
};

/* Used to expose a hardware BAR (or logical slice of it) outside skiboot */
struct pau_bar {
	bool			enable;
	uint64_t		addr;
	uint64_t		size;
	uint64_t		cfg;
};

struct pau_dev {
	enum pau_dev_type	type;
	uint32_t		index;
	struct dt_node		*dn;
	struct phb		phb;

	struct pau_bar		ntl_bar;
	struct pau_bar		genid_bar;

	/* Associated I2C information */
	uint8_t			i2c_bus_id;

	/* Associated PHY information */
	uint32_t		pau_unit; /* 0,3,4,5,6,7 */
	uint32_t		odl_index;
	uint32_t		op_unit; /* 0 -> 7 */
	uint32_t		phy_lane_mask;

	struct pau		*pau;
};

struct pau {
	uint32_t		index;
	struct dt_node		*dt_node;
	uint32_t		chip_id;
	uint64_t		xscom_base;

	/* Global MMIO window (all PAU regs) */
	uint64_t		regs[2];
	bool			mmio_access;

	struct lock		lock;

	uint32_t		links;
	struct pau_dev		devices[PAU_LINKS_OPENCAPI_PER_PAU];
};

#define PAUDBG(pau, fmt, a...) PAULOG(PR_DEBUG, pau, fmt, ##a)
#define PAUINF(pau, fmt, a...) PAULOG(PR_INFO, pau, fmt, ##a)
#define PAUERR(pau, fmt, a...) PAULOG(PR_ERR, pau, fmt, ##a)

#define PAUDEVDBG(dev, fmt, a...) PAUDEVLOG(PR_DEBUG, dev, fmt, ##a)
#define PAUDEVINF(dev, fmt, a...) PAUDEVLOG(PR_INFO, dev, fmt, ##a)
#define PAUDEVERR(dev, fmt, a...) PAUDEVLOG(PR_ERR, dev, fmt, ##a)

#define PAULOG(l, pau, fmt, a...) \
	prlog(l, "PAU[%d:%d]: " fmt, (pau)->chip_id, (pau)->index, ##a)

#define PAUDEVLOG(l, dev, fmt, a...)		\
	prlog(l, "PAU[%d:%d:%d]: " fmt,		\
	      (dev)->pau->chip_id,		\
	      (dev)->pau->index,		\
	      (dev)->index, ##a)


/* pau-scope index of the link */
static inline uint32_t pau_dev_index(struct pau_dev *dev, int links)
{
	return dev->pau->index * links + dev->index;
}

static inline struct pau_dev *pau_phb_to_opencapi_dev(struct phb *phb)
{
	assert(phb->phb_type == phb_type_pau_opencapi);
	return container_of(phb, struct pau_dev, phb);
}

struct pau_dev *pau_next_dev(struct pau *pau, struct pau_dev *dev,
			       enum pau_dev_type type);

#define pau_for_each_dev_type(dev, pau, type) \
	for (dev = NULL; (dev = pau_next_dev(pau, dev, type));)

#define pau_for_each_opencapi_dev(dev, pau) \
	pau_for_each_dev_type(dev, pau, PAU_DEV_TYPE_OPENCAPI)

#define pau_for_each_dev(dev, pau) \
	pau_for_each_dev_type(dev, pau, PAU_DEV_TYPE_ANY)

#define PAU_PHB_INDEX_BASE	6 /* immediately after real PHBs */
static inline int pau_get_phb_index(unsigned int pau_index,
				    unsigned int link_index)
{
	return PAU_PHB_INDEX_BASE + pau_index * 2 + link_index;
}

static inline int pau_get_opal_id(unsigned int chip_id, unsigned int index)
{
	return phb4_get_opal_id(chip_id, index);
}

/*
 * We use the indirect method because it uses the same addresses as
 * the MMIO offsets (PAU RING)
 */
static inline void pau_scom_sel(struct pau *pau, uint64_t reg,
				uint64_t size)
{
	uint64_t val;

	val = SETFIELD(PAU_MISC_DA_ADDR, 0ull, reg);
	val = SETFIELD(PAU_MISC_DA_LEN, val, size);
	xscom_write(pau->chip_id,
		    pau->xscom_base + PAU_MISC_SCOM_IND_SCOM_ADDR,
		    val);
}

static inline void pau_scom_write(struct pau *pau, uint64_t reg,
				  uint64_t size,
				  uint64_t val)
{
	pau_scom_sel(pau, reg, size);
	xscom_write(pau->chip_id,
		    pau->xscom_base + PAU_MISC_SCOM_IND_SCOM_DATA,
		    val);
}

static inline uint64_t pau_scom_read(struct pau *pau, uint64_t reg,
				     uint64_t size)
{
	uint64_t val;

	pau_scom_sel(pau, reg, size);
	xscom_read(pau->chip_id,
		   pau->xscom_base + PAU_MISC_SCOM_IND_SCOM_DATA,
		   &val);

	return val;
}

static inline void pau_write(struct pau *pau, uint64_t reg,
			     uint64_t val)
{
	void *mmio = (void *)pau->regs[0];

	if (pau->mmio_access)
		out_be64(mmio + reg, val);
	else
		pau_scom_write(pau, reg, PAU_MISC_DA_LEN_8B, val);

	/* CQ_SM writes should be mirrored in all four blocks */
	if (PAU_REG_BLOCK(reg) != PAU_BLOCK_CQ_SM(0))
		return;

	for (uint32_t i = 1; i < 4; i++)
		pau_write(pau, PAU_BLOCK_CQ_SM(i) + PAU_REG_OFFSET(reg),
			   val);
}

static inline uint64_t pau_read(struct pau *pau, uint64_t reg)
{
	void *mmio = (void *)pau->regs[0];

	if (pau->mmio_access)
		return in_be64(mmio + reg);

	return pau_scom_read(pau, reg, PAU_MISC_DA_LEN_8B);
}

#endif /* __PAU_H */