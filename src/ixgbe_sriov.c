/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 1999 - 2025 Intel Corporation */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/ipv6.h>

#include "ixgbe.h"
#include "ixgbe_type.h"
#include "ixgbe_sriov.h"

static void ixgbe_set_vf_rx_tx(struct ixgbe_adapter *adapter, int vf);

#ifdef CONFIG_PCI_IOV
static inline void ixgbe_alloc_vf_macvlans(struct ixgbe_adapter *adapter,
					   unsigned int num_vfs)
{
	struct ixgbe_hw *hw = &adapter->hw;
	struct vf_macvlans *mv_list;
	int num_vf_macvlans, i;

	num_vf_macvlans = hw->mac.num_rar_entries -
			  (IXGBE_MAX_PF_MACVLANS + 1 + num_vfs);
	if (!num_vf_macvlans)
		return;

	mv_list = kcalloc(num_vf_macvlans, sizeof(struct vf_macvlans),
			  GFP_KERNEL);
	if (mv_list) {
		/* Initialize list of VF macvlans */
		INIT_LIST_HEAD(&adapter->vf_mvs.l);
		for (i = 0; i < num_vf_macvlans; i++) {
			mv_list[i].vf = -1;
			mv_list[i].free = true;
			list_add(&mv_list[i].l, &adapter->vf_mvs.l);
		}
		adapter->mv_list = mv_list;
	}
}

static int __ixgbe_enable_sriov(struct ixgbe_adapter *adapter,
				unsigned int num_vfs)
{
	struct ixgbe_hw *hw = &adapter->hw;
	int i;

#ifdef HAVE_XDP_SUPPORT
	if (adapter->xdp_prog) {
		e_warn(probe, "SRIOV is not supported with XDP\n");
		return -EINVAL;
	}
#endif /* HAVE_XDP_SUPPORT */

	adapter->flags |= IXGBE_FLAG_SRIOV_ENABLED;

	/* Enable VMDq flag so device will be set in VM mode */
	adapter->flags |= IXGBE_FLAG_VMDQ_ENABLED;
	if (!adapter->ring_feature[RING_F_VMDQ].limit)
		adapter->ring_feature[RING_F_VMDQ].limit = 1;

	/* Allocate memory for per VF control structures */
	adapter->vfinfo = kcalloc(num_vfs, sizeof(struct vf_data_storage),
				  GFP_KERNEL);
	if (!adapter->vfinfo)
		return -ENOMEM;

	/* Initialize default switching mode VEB */
	IXGBE_WRITE_REG(hw, IXGBE_PFDTXGSWC, IXGBE_PFDTXGSWC_VT_LBEN);

	/* set adapter->num_vfs only after allocating vfinfo to avoid
	 * NULL pointer issues when accessing adapter->vfinfo
	 */
	adapter->num_vfs = num_vfs;

	ixgbe_alloc_vf_macvlans(adapter, num_vfs);

	adapter->ring_feature[RING_F_VMDQ].offset = num_vfs;

	/* enable L2 switch and replication */
	adapter->flags |= IXGBE_FLAG_SRIOV_L2SWITCH_ENABLE |
				IXGBE_FLAG_SRIOV_REPLICATION_ENABLE;

	/* limit traffic classes based on VFs enabled */
	if ((adapter->hw.mac.type == ixgbe_mac_82599EB) &&
		(adapter->num_vfs < 16)) {
		adapter->dcb_cfg.num_tcs.pg_tcs =
			IXGBE_DCB_MAX_TRAFFIC_CLASS;
		adapter->dcb_cfg.num_tcs.pfc_tcs =
			IXGBE_DCB_MAX_TRAFFIC_CLASS;
	} else if (adapter->num_vfs < 32) {
		adapter->dcb_cfg.num_tcs.pg_tcs = 4;
		adapter->dcb_cfg.num_tcs.pfc_tcs = 4;
	} else {
		adapter->dcb_cfg.num_tcs.pg_tcs = 1;
		adapter->dcb_cfg.num_tcs.pfc_tcs = 1;
	}
	adapter->dcb_cfg.vt_mode = true;

#ifdef IXGBE_DISABLE_VF_MQ
	/* We do not support RSS w/ SR-IOV */
	adapter->ring_feature[RING_F_RSS].limit = 1;
#endif

	/* Disable RSC when in SR-IOV mode */
	adapter->flags2 &= ~(IXGBE_FLAG2_RSC_CAPABLE |
				IXGBE_FLAG2_RSC_ENABLED);


	for (i = 0; i < adapter->num_vfs; i++) {
		/* enable spoof checking for all VFs */
		adapter->vfinfo[i].spoofchk_enabled = true;
		adapter->vfinfo[i].link_enable = true;

#ifdef HAVE_NDO_SET_VF_RSS_QUERY_EN
		/* We support VF RSS querying only for 82599 and x540
		 * devices at the moment. These devices share RSS
		 * indirection table and RSS hash key with PF therefore
		 * we want to disable the querying by default.
		 */
		adapter->vfinfo[i].rss_query_enabled = 0;

#endif
		/* Untrust all VFs */
		adapter->vfinfo[i].trusted = false;

		/* set the default xcast mode */
		adapter->vfinfo[i].xcast_mode = IXGBEVF_XCAST_MODE_NONE;
	}

	e_dev_info("SR-IOV enabled with %d VFs\n", num_vfs);
	if (hw->mac.type < ixgbe_mac_X550)
		e_dev_info("configure port vlans to keep your VFs secure\n");

	return 0;
}

/**
 * ixgbe_get_vfs - Find and take references to all vf devices
 * @adapter: Pointer to adapter struct
 */
static void ixgbe_get_vfs(struct ixgbe_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	u16 vendor = pdev->vendor;
	struct pci_dev *vfdev;
	int vf = 0;
	u16 vf_id;
	int pos;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_SRIOV);
	if (!pos)
		return;
	pci_read_config_word(pdev, pos + PCI_SRIOV_VF_DID, &vf_id);

	vfdev = pci_get_device(vendor, vf_id, NULL);
	for (; vfdev; vfdev = pci_get_device(vendor, vf_id, vfdev)) {
		if (!vfdev->is_virtfn)
			continue;
		if (vfdev->physfn != pdev)
			continue;
		if (vf >= adapter->num_vfs)
			continue;
		pci_dev_get(vfdev);
		adapter->vfinfo[vf].vfdev = vfdev;
		++vf;
	}
}

/* Note this function is called when the user wants to enable SR-IOV
 * VFs using the now deprecated module parameter
 */
void ixgbe_enable_sriov(struct ixgbe_adapter *adapter)
{
	int pre_existing_vfs = 0;
	unsigned int num_vfs;

	pre_existing_vfs = pci_num_vf(adapter->pdev);
	if (!pre_existing_vfs && !adapter->max_vfs)
		return;

	/* If there are pre-existing VFs then we have to force
	 * use of that many - over ride any module parameter value.
	 * This may result from the user unloading the PF driver
	 * while VFs were assigned to guest VMs or because the VFs
	 * have been created via the new PCI SR-IOV sysfs interface.
	 */
	if (pre_existing_vfs) {
		num_vfs = pre_existing_vfs;
		dev_warn(&adapter->pdev->dev,
			 "Virtual Functions already enabled for this device - Please reload all VF drivers to avoid spoofed packet errors\n");
	} else {
		int err;
		/*
		 * The 82599 supports up to 64 VFs per physical function
		 * but this implementation limits allocation to 63 so that
		 * basic networking resources are still available to the
		 * physical function.  If the user requests greater thn
		 * 63 VFs then it is an error - reset to default of zero.
		 */
		num_vfs = min_t(unsigned int, adapter->max_vfs,
				IXGBE_MAX_VFS_DRV_LIMIT);

		err = pci_enable_sriov(adapter->pdev, num_vfs);
		if (err) {
			e_err(probe, "Failed to enable PCI sriov: %d\n", err);
			return;
		}
	}

	if (!__ixgbe_enable_sriov(adapter, num_vfs)) {
		ixgbe_get_vfs(adapter);
		return;
	}

	/* If we have gotten to this point then there is no memory available
	 * to manage the VF devices - print message and bail.
	 */
	e_err(probe, "Unable to allocate memory for VF Data Storage - SRIOV disabled\n");
	ixgbe_disable_sriov(adapter);
}

#endif /* CONFIG_PCI_IOV */
int ixgbe_disable_sriov(struct ixgbe_adapter *adapter)
{
	unsigned int num_vfs = adapter->num_vfs, vf;
	struct ixgbe_hw *hw = &adapter->hw;
	u32 gpie;
	u32 vmdctl;

	/* set num VFs to 0 to prevent access to vfinfo */
	adapter->num_vfs = 0;

	/* put the reference to all of the vf devices */
	for (vf = 0; vf < num_vfs; ++vf) {
		struct pci_dev *vfdev = adapter->vfinfo[vf].vfdev;

		if (!vfdev)
			continue;
		adapter->vfinfo[vf].vfdev = NULL;
		pci_dev_put(vfdev);
	}

	/* free VF control structures */
	kfree(adapter->vfinfo);
	adapter->vfinfo = NULL;

	/* free macvlan list */
	kfree(adapter->mv_list);
	adapter->mv_list = NULL;

	/* if SR-IOV is already disabled then there is nothing to do */
	if (!(adapter->flags & IXGBE_FLAG_SRIOV_ENABLED))
		return 0;

	/* Turn off malicious driver detection */
	if ((hw->mac.ops.disable_mdd) &&
		(!(adapter->flags & IXGBE_FLAG_MDD_ENABLED)))
		hw->mac.ops.disable_mdd(hw);

#ifdef CONFIG_PCI_IOV
	/*
	 * If our VFs are assigned we cannot shut down SR-IOV
	 * without causing issues, so just leave the hardware
	 * available but disabled
	 */
	if (pci_vfs_assigned(adapter->pdev)) {
		e_dev_warn("Unloading driver while VFs are assigned - VFs will not be deallocated\n");
		return -EPERM;
	}
	/* disable iov and allow time for transactions to clear */
	pci_disable_sriov(adapter->pdev);
#endif
	/* turn off device IOV mode */
	IXGBE_WRITE_REG(hw, IXGBE_GCR_EXT, 0);
	gpie = IXGBE_READ_REG(hw, IXGBE_GPIE);
	gpie &= ~IXGBE_GPIE_VTMODE_MASK;
	IXGBE_WRITE_REG(hw, IXGBE_GPIE, gpie);

	/* set default pool back to 0 */
	vmdctl = IXGBE_READ_REG(hw, IXGBE_VT_CTL);
	vmdctl &= ~IXGBE_VT_CTL_POOL_MASK;
	IXGBE_WRITE_REG(hw, IXGBE_VT_CTL, vmdctl);
	IXGBE_WRITE_FLUSH(hw);

	/* Disable VMDq flag so device will be set in VM mode */
	if (adapter->ring_feature[RING_F_VMDQ].limit == 1)
		adapter->flags &= ~IXGBE_FLAG_VMDQ_ENABLED;

	adapter->flags &= ~IXGBE_FLAG_SRIOV_ENABLED;
	adapter->ring_feature[RING_F_VMDQ].offset = 0;

	/* take a breather then clean up driver data */
	msleep(100);
	return 0;
}

static int ixgbe_pci_sriov_enable(struct pci_dev __maybe_unused *dev, int __maybe_unused num_vfs)
{
#ifdef CONFIG_PCI_IOV
	struct ixgbe_adapter *adapter = pci_get_drvdata(dev);
	int pre_existing_vfs = pci_num_vf(dev);
	int err = 0, i;
	u8 num_tc;

	if (!(adapter->flags & IXGBE_FLAG_SRIOV_CAPABLE)) {
		e_dev_warn("SRIOV not supported on this device\n");
		return -EOPNOTSUPP;
	}

	if (adapter->num_vfs == num_vfs)
		return -EINVAL;

	if (pre_existing_vfs && pre_existing_vfs != num_vfs)
		err = ixgbe_disable_sriov(adapter);
	else if (pre_existing_vfs && pre_existing_vfs == num_vfs)
		goto out;

	if (err)
		goto err_out;

	/* While the SR-IOV capability structure reports total VFs to be
	 * 64 we limit the actual number that can be allocated as below
	 * so that some transmit/receive resources can be reserved to the
	 * PF.  The PCI bus driver already checks for other values out of
	 * range.
	 *    Num_TCs	MAX_VFs
	 *	1	  63
	 *	<=4	  31
	 *	>4	  15
	 */
	num_tc = netdev_get_num_tc(adapter->netdev);
	if (num_tc > 4) {
		if (num_vfs > IXGBE_MAX_VFS_8TC) {
			e_dev_err("Currently the device is configured with %d TCs, Creating more than %d VFs is not allowed\n", num_tc, IXGBE_MAX_VFS_8TC);
			err = -EPERM;
			goto err_out;
		}
	} else if ((num_tc > 1) && (num_tc <= 4)) {
		if (num_vfs > IXGBE_MAX_VFS_4TC) {
			e_dev_err("Currently the device is configured with %d TCs, Creating more than %d VFs is not allowed\n", num_tc, IXGBE_MAX_VFS_4TC);
			err = -EPERM;
			goto err_out;
		}
	} else {
		if (num_vfs > IXGBE_MAX_VFS_1TC) {
			e_dev_err("Currently the device is configured with %d TCs, Creating more than %d VFs is not allowed\n", num_tc, IXGBE_MAX_VFS_1TC);
			err = -EPERM;
			goto err_out;
		}
	}

	err = __ixgbe_enable_sriov(adapter, num_vfs);
	if (err)
		goto err_out;

	for (i = 0; i < adapter->num_vfs; i++)
		ixgbe_vf_configuration(dev, (i | 0x10000000));

	/* reset before enabling SRIOV to avoid mailbox issues */
	ixgbe_sriov_reinit(adapter);

	err = pci_enable_sriov(dev, num_vfs);
	if (err) {
		e_dev_warn("Failed to enable PCI sriov: %d\n", err);
		goto err_out;
	}
	ixgbe_get_vfs(adapter);

out:
	return num_vfs;

err_out:
	return err;
#endif
	return 0;
}

static int ixgbe_pci_sriov_disable(struct pci_dev *dev)
{
	struct ixgbe_adapter *adapter = pci_get_drvdata(dev);
	int err;
#ifdef CONFIG_PCI_IOV
	u32 current_flags = adapter->flags;
#endif

	if (!adapter->num_vfs && !pci_num_vf(dev))
		return -EINVAL;

	err = ixgbe_disable_sriov(adapter);

	/* Only reinit if no error and state changed */
#ifdef CONFIG_PCI_IOV
	if (!err && current_flags != adapter->flags)
		ixgbe_sriov_reinit(adapter);
#endif

	return err;
}

int ixgbe_pci_sriov_configure(struct pci_dev *dev, int num_vfs)
{
	if (num_vfs == 0)
		return ixgbe_pci_sriov_disable(dev);
	else
		return ixgbe_pci_sriov_enable(dev, num_vfs);
}

static int ixgbe_set_vf_multicasts(struct ixgbe_adapter *adapter,
				   u32 *msgbuf, u32 vf)
{
	int entries = (msgbuf[0] & IXGBE_VT_MSGINFO_MASK)
		       >> IXGBE_VT_MSGINFO_SHIFT;
	u16 *hash_list = (u16 *)&msgbuf[1];
	struct vf_data_storage *vfinfo = &adapter->vfinfo[vf];
	struct ixgbe_hw *hw = &adapter->hw;
	int i;
	u32 vmolr = IXGBE_READ_REG(hw, IXGBE_VMOLR(vf));

	/* only so many hash values supported */
	entries = min(entries, IXGBE_MAX_VF_MC_ENTRIES);

	/* salt away the number of multi cast addresses assigned
	 * to this VF for later use to restore when the PF multi cast
	 * list changes
	 */
	vfinfo->num_vf_mc_hashes = entries;

	/* VFs are limited to using the MTA hash table for their multicast
	 * addresses */
	for (i = 0; i < entries; i++)
		vfinfo->vf_mc_hashes[i] = hash_list[i];

	vmolr |= IXGBE_VMOLR_ROMPE;
	IXGBE_WRITE_REG(hw, IXGBE_VMOLR(vf), vmolr);

	/* Sync up the PF and VF in the same MTA table */
	ixgbe_write_mc_addr_list(adapter->netdev);

	return 0;
}

#ifdef CONFIG_PCI_IOV
void ixgbe_restore_vf_multicasts(struct ixgbe_adapter *adapter)
{
	struct ixgbe_hw *hw = &adapter->hw;
	struct vf_data_storage *vfinfo;
	int i, j;
	u32 vector_bit;
	u32 vector_reg;

	/* Clear mta_shadow */
	memset(&hw->mac.mta_shadow, 0, sizeof(hw->mac.mta_shadow));

	for (i = 0; i < adapter->num_vfs; i++) {
		u32 vmolr = IXGBE_READ_REG(hw, IXGBE_VMOLR(i));
		vfinfo = &adapter->vfinfo[i];
		for (j = 0; j < vfinfo->num_vf_mc_hashes; j++) {
			hw->addr_ctrl.mta_in_use++;
			vector_reg = (vfinfo->vf_mc_hashes[j] >> 5) & 0x7F;
			vector_bit = vfinfo->vf_mc_hashes[j] & 0x1F;
			hw->mac.mta_shadow[vector_reg] |= (1 << vector_bit);
		}
		if (vfinfo->num_vf_mc_hashes)
			vmolr |= IXGBE_VMOLR_ROMPE;
		else
			vmolr &= ~IXGBE_VMOLR_ROMPE;
		IXGBE_WRITE_REG(hw, IXGBE_VMOLR(i), vmolr);
	}

	/* Restore any VF macvlans */
	ixgbe_full_sync_mac_table(adapter);
}
#endif /* CONFIG_PCI_IOV */

int ixgbe_set_vf_vlan(struct ixgbe_adapter *adapter, int add, int vid, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	int err;

#ifndef HAVE_VLAN_RX_REGISTER
	/* If VLAN overlaps with one the PF is currently monitoring make
	 * sure that we are able to allocate a VLVF entry.  This may be
	 * redundant but it guarantees PF will maintain visibility to
	 * the VLAN.
	 */
	if (add && test_bit(vid, adapter->active_vlans)) {
		err = hw->mac.ops.set_vfta(hw, vid, VMDQ_P(0), true, false);
		if (err)
			return err;
	}
#endif

	err = hw->mac.ops.set_vfta(hw, vid, vf, !!add, false);
#ifndef HAVE_VLAN_RX_REGISTER

	if (add && !err)
		return err;

	/* If we failed to add the VF VLAN or we are removing the VF VLAN
	 * we may need to drop the PF pool bit in order to allow us to free
	 * up the VLVF resources.
	 */
	if (test_bit(vid, adapter->active_vlans) ||
	    (adapter->flags2 & IXGBE_FLAG2_VLAN_PROMISC))
		ixgbe_update_pf_promisc_vlvf(adapter, vid);
#endif

	return err;
}
static int ixgbe_set_vf_lpe(struct ixgbe_adapter *adapter, u32 max_frame, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 max_frs;

	/*
	 * For 82599EB we have to keep all PFs and VFs operating with
	 * the same max_frame value in order to avoid sending an oversize
	 * frame to a VF.  In order to guarantee this is handled correctly
	 * for all cases we have several special exceptions to take into
	 * account before we can enable the VF for receive
	 */
	if (adapter->hw.mac.type == ixgbe_mac_82599EB) {

		struct net_device *dev = adapter->netdev;
		int pf_max_frame = dev->mtu + ETH_HLEN;
		u32 reg_offset, vf_shift, vfre;
		s32 err = 0;

#if IS_ENABLED(CONFIG_FCOE)
		if (dev->features & NETIF_F_FCOE_MTU)
			pf_max_frame = max_t(int, pf_max_frame,
					     IXGBE_FCOE_JUMBO_FRAME_SIZE);
#endif /* CONFIG_FCOE */

		switch (adapter->vfinfo[vf].vf_api) {
		case ixgbe_mbox_api_11:
		case ixgbe_mbox_api_12:
		case ixgbe_mbox_api_13:
			/* Version 1.1 supports jumbo frames on VFs if PF has
			 * jumbo frames enabled which means legacy VFs are
			 * disabled
			 */
			if (pf_max_frame > ETH_FRAME_LEN)
				break;
			fallthrough;
		default:
			/* If the PF or VF are running w/ jumbo frames enabled
			 * we need to shut down the VF Rx path as we cannot
			 * support jumbo frames on legacy VFs
			 */
			if ((pf_max_frame > ETH_FRAME_LEN) ||
			    (max_frame > (ETH_FRAME_LEN + ETH_FCS_LEN)))
				err = -EINVAL;
			break;
		}

		/* determine VF receive enable location */
		vf_shift = vf % 32;
		reg_offset = vf / 32;

		/* enable or disable receive depending on error */
		vfre = IXGBE_READ_REG(hw, IXGBE_VFRE(reg_offset));
		if (err)
			vfre &= ~(1 << vf_shift);
		else
			vfre |= 1 << vf_shift;
		IXGBE_WRITE_REG(hw, IXGBE_VFRE(reg_offset), vfre);

		if (err) {
			e_err(drv, "VF max_frame %d out of range\n", max_frame);
			return err;
		}
	}

	/* pull current max frame size from hardware */
	max_frs = IXGBE_READ_REG(hw, IXGBE_MAXFRS);
	max_frs &= IXGBE_MHADD_MFS_MASK;
	max_frs >>= IXGBE_MHADD_MFS_SHIFT;

	if (max_frs < max_frame) {
		max_frs = max_frame << IXGBE_MHADD_MFS_SHIFT;
		IXGBE_WRITE_REG(hw, IXGBE_MAXFRS, max_frs);
	}

	e_info(hw, "VF requests change max MTU to %d\n", max_frame);

	return 0;
}

void ixgbe_set_vmolr(struct ixgbe_hw *hw, u32 vf, bool aupe)
{
	u32 vmolr = IXGBE_READ_REG(hw, IXGBE_VMOLR(vf));
	vmolr |=  IXGBE_VMOLR_BAM;
	if (aupe)
		vmolr |= IXGBE_VMOLR_AUPE;
	else
		vmolr &= ~IXGBE_VMOLR_AUPE;
	IXGBE_WRITE_REG(hw, IXGBE_VMOLR(vf), vmolr);
}

static void ixgbe_set_vmvir(struct ixgbe_adapter *adapter,
			    u16 vid, u16 qos, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 vmvir = vid | (qos << VLAN_PRIO_SHIFT) | IXGBE_VMVIR_VLANA_DEFAULT;

	IXGBE_WRITE_REG(hw, IXGBE_VMVIR(vf), vmvir);
}

static void ixgbe_clear_vmvir(struct ixgbe_adapter *adapter, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;

	IXGBE_WRITE_REG(hw, IXGBE_VMVIR(vf), 0);
}

static void ixgbe_clear_vf_vlans(struct ixgbe_adapter *adapter, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 vlvfb_mask, pool_mask, i;

	/* create mask for VF and other pools */
	pool_mask = (u32)~BIT(VMDQ_P(0) % 32);
	vlvfb_mask = BIT(vf % 32);

	/* post increment loop, covers VLVF_ENTRIES - 1 to 0 */
	for (i = IXGBE_VLVF_ENTRIES; i--;) {
		u32 bits[2], vlvfb, vid, vfta, vlvf;
		u32 word = i * 2 + vf / 32;
		u32 mask;

		vlvfb = IXGBE_READ_REG(hw, IXGBE_VLVFB(word));

		/* if our bit isn't set we can skip it */
		if (!(vlvfb & vlvfb_mask))
			continue;

		/* clear our bit from vlvfb */
		vlvfb ^= vlvfb_mask;

		/* create 64b mask to check to see if we should clear VLVF */
		bits[word % 2] = vlvfb;
		bits[~word % 2] = IXGBE_READ_REG(hw, IXGBE_VLVFB(word ^ 1));

		/* if other pools are present, just remove ourselves */
		if (bits[(VMDQ_P(0) / 32) ^ 1] ||
		    (bits[VMDQ_P(0) / 32] & pool_mask))
			goto update_vlvfb;

		/* if PF is present, leave VFTA */
		if (bits[0] || bits[1])
			goto update_vlvf;

		/* if we cannot determine VLAN just remove ourselves */
		vlvf = IXGBE_READ_REG(hw, IXGBE_VLVF(i));
		if (!vlvf)
			goto update_vlvfb;

		vid = vlvf & VLAN_VID_MASK;
		mask = BIT(vid % 32);

		/* clear bit from VFTA */
		vfta = IXGBE_READ_REG(hw, IXGBE_VFTA(vid / 32));
		if (vfta & mask)
			IXGBE_WRITE_REG(hw, IXGBE_VFTA(vid / 32), vfta ^ mask);
update_vlvf:
		/* clear POOL selection enable */
		IXGBE_WRITE_REG(hw, IXGBE_VLVF(i), 0);

		if (!(adapter->flags2 & IXGBE_FLAG2_VLAN_PROMISC))
			vlvfb = 0;
update_vlvfb:
		/* clear pool bits */
		IXGBE_WRITE_REG(hw, IXGBE_VLVFB(word), vlvfb);
	}
}

static int ixgbe_set_vf_macvlan(struct ixgbe_adapter *adapter,
				int vf, int index, unsigned char *mac_addr)
{
	struct vf_macvlans *entry;
	struct list_head *pos;
	int retval = 0;

	if (index <= 1) {
		list_for_each(pos, &adapter->vf_mvs.l) {
			entry = list_entry(pos, struct vf_macvlans, l);
			if (entry->vf == vf) {
				entry->vf = -1;
				entry->free = true;
				entry->is_macvlan = false;
				ixgbe_del_mac_filter(adapter,
						     entry->vf_macvlan, vf);
			}
		}
	}

	/*
	 * If index was zero then we were asked to clear the uc list
	 * for the VF.  We're done.
	 */
	if (!index)
		return 0;

	entry = NULL;

	list_for_each(pos, &adapter->vf_mvs.l) {
		entry = list_entry(pos, struct vf_macvlans, l);
		if (entry->free)
			break;
	}

	/*
	 * If we traversed the entire list and didn't find a free entry
	 * then we're out of space on the RAR table.  Also entry may
	 * be NULL because the original memory allocation for the list
	 * failed, which is not fatal but does mean we can't support
	 * VF requests for MACVLAN because we couldn't allocate
	 * memory for the list management required.
	 */
	if (!entry || !entry->free)
		return -ENOSPC;

	retval = ixgbe_add_mac_filter(adapter, mac_addr, vf);
	if (retval < 0)
		return retval;

	entry->free = false;
	entry->is_macvlan = true;
	entry->vf = vf;
	memcpy(entry->vf_macvlan, mac_addr, ETH_ALEN);

	return 0;
}

static inline void ixgbe_vf_reset_event(struct ixgbe_adapter *adapter, u32 vf)
{
	struct ixgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	struct vf_data_storage *vfinfo = &adapter->vfinfo[vf];
	u8 num_tcs = netdev_get_num_tc(adapter->netdev);
	u32 q_per_pool = __ALIGN_MASK(1, ~vmdq->mask);
	struct ixgbe_hw *hw = &adapter->hw;
	u32 reg_val;
	u32 queue;
	u32 word;

	/* remove VLAN filters belonging to this VF */
	ixgbe_clear_vf_vlans(adapter, vf);

	/* add back PF assigned VLAN or VLAN 0 */
	ixgbe_set_vf_vlan(adapter, true, vfinfo->pf_vlan, vf);

	/* reset offloads to defaults */
	ixgbe_set_vmolr(hw, vf, !vfinfo->pf_vlan);

	/* set outgoing tags for VFs */
	if (!vfinfo->pf_vlan && !vfinfo->pf_qos && !num_tcs) {
		ixgbe_clear_vmvir(adapter, vf);
	} else {
		if (vfinfo->pf_qos || !num_tcs)
			ixgbe_set_vmvir(adapter, vfinfo->pf_vlan,
					vfinfo->pf_qos, vf);
		else
			ixgbe_set_vmvir(adapter, vfinfo->pf_vlan,
					adapter->default_up, vf);
	}

	/* reset multicast table array for vf */
	adapter->vfinfo[vf].num_vf_mc_hashes = 0;

	/* Flush and reset the mta with the new values */
	ixgbe_set_rx_mode(adapter->netdev);

	ixgbe_del_mac_filter(adapter, adapter->vfinfo[vf].vf_mac_addresses, vf);
	ixgbe_set_vf_macvlan(adapter, vf, 0, NULL);

	/* reset VF api back to unknown */
	adapter->vfinfo[vf].vf_api = ixgbe_mbox_api_10;

	/*
	 * Toggling VF's TX queues and clearing VF Mailbox Memory after VFLR
	 * should only affect X550 and above
	 */
	if (hw->mac.type >= ixgbe_mac_X550) {
		/* Restart each queue for given VF */
		for (queue = 0; queue < q_per_pool; queue++) {
			unsigned int reg_idx = (vf * q_per_pool) + queue;

			reg_val = IXGBE_READ_REG(hw, IXGBE_PVFTXDCTL(reg_idx));

			/* Re-enabling only configured queues */
			if (reg_val) {
				reg_val |= IXGBE_TXDCTL_ENABLE;
				IXGBE_WRITE_REG(hw, IXGBE_PVFTXDCTL(reg_idx),
						reg_val);
				reg_val &= ~IXGBE_TXDCTL_ENABLE;
				IXGBE_WRITE_REG(hw, IXGBE_PVFTXDCTL(reg_idx),
						reg_val);
			}
		}

		/* Clear VF's mailbox memory */
		for (word = 0; word < IXGBE_VFMAILBOX_SIZE; word++)
			IXGBE_WRITE_REG_ARRAY(hw, IXGBE_PFMBMEM(vf), word, 0);

		IXGBE_WRITE_FLUSH(hw);
	}
}

int ixgbe_set_vf_mac(struct ixgbe_adapter *adapter,
		     int vf, unsigned char *mac_addr)
{
	s32 retval = 0;

	ixgbe_del_mac_filter(adapter, adapter->vfinfo[vf].vf_mac_addresses, vf);
	retval = ixgbe_add_mac_filter(adapter, mac_addr, vf);
	if (retval >= 0)
		memcpy(adapter->vfinfo[vf].vf_mac_addresses,
		       mac_addr, ETH_ALEN);
	else
		memset(adapter->vfinfo[vf].vf_mac_addresses, 0, ETH_ALEN);

	return retval;
}

#ifdef CONFIG_PCI_IOV
int ixgbe_vf_configuration(struct pci_dev *pdev, unsigned int event_mask)
{
	unsigned char vf_mac_addr[6];
	struct ixgbe_adapter *adapter = pci_get_drvdata(pdev);
	unsigned int vfn = (event_mask & 0x3f);
	bool enable = ((event_mask & 0x10000000U) != 0);

	if (enable) {
		memset(vf_mac_addr, 0, ETH_ALEN);
		memcpy(adapter->vfinfo[vfn].vf_mac_addresses, vf_mac_addr, 6);
	}

	return 0;
}
#endif /* CONFIG_PCI_IOV */

static inline void ixgbe_write_qde(struct ixgbe_adapter *adapter, u32 vf,
				   u32 qde)
{
	struct ixgbe_hw *hw = &adapter->hw;
	struct ixgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	u32 q_per_pool = __ALIGN_MASK(1, ~vmdq->mask);
	u32 reg;
	int i;

	for (i = vf * q_per_pool; i < ((vf + 1) * q_per_pool); i++) {
		/* flush previous write */
		IXGBE_WRITE_FLUSH(hw);

		/* drop enable should always be set in SRIOV mode*/
		reg = IXGBE_QDE_WRITE | qde;
		reg |= i <<  IXGBE_QDE_IDX_SHIFT;
		IXGBE_WRITE_REG(hw, IXGBE_QDE, reg);
	}

}

static int ixgbe_vf_reset_msg(struct ixgbe_adapter *adapter, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	struct ixgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	unsigned char *vf_mac = adapter->vfinfo[vf].vf_mac_addresses;
	u32 reg, reg_offset, vf_shift;
	u32 msgbuf[4] = {0, 0, 0, 0};
	u8 *addr = (u8 *)(&msgbuf[1]);
	u32 q_per_pool = __ALIGN_MASK(1, ~vmdq->mask);
	int i;

	e_info(probe, "VF Reset msg received from vf %d\n", vf);

	/* reset the filters for the device */
	ixgbe_vf_reset_event(adapter, vf);

	/* set vf mac address */
	if (!is_zero_ether_addr(vf_mac))
		ixgbe_set_vf_mac(adapter, vf, vf_mac);

	vf_shift = vf % 32;
	reg_offset = vf / 32;

	/* force drop enable for all VF Rx queues */
	reg = IXGBE_QDE_ENABLE;
	if (adapter->vfinfo[vf].pf_vlan)
		reg |= IXGBE_QDE_HIDE_VLAN;

	ixgbe_write_qde(adapter, vf, reg);

	/* set transmit and receive for vf */
	ixgbe_set_vf_rx_tx(adapter, vf);

	/* enable VF mailbox for further messages */
	adapter->vfinfo[vf].clear_to_send = true;

	reg = IXGBE_READ_REG(hw, IXGBE_VMECM(reg_offset));
	reg |= (1 << vf_shift);
	IXGBE_WRITE_REG(hw, IXGBE_VMECM(reg_offset), reg);

	/*
	 * Reset the VFs TDWBAL and TDWBAH registers
	 * which are not cleared by an FLR
	 */
	for (i = 0; i < q_per_pool; i++) {
		IXGBE_WRITE_REG(hw, IXGBE_PVFTDWBAHn(q_per_pool, vf, i), 0);
		IXGBE_WRITE_REG(hw, IXGBE_PVFTDWBALn(q_per_pool, vf, i), 0);
	}

	/* reply to reset with ack and vf mac address */
	msgbuf[0] = IXGBE_VF_RESET;
	if (!is_zero_ether_addr(vf_mac) && adapter->vfinfo[vf].pf_set_mac) {
		msgbuf[0] |= IXGBE_VT_MSGTYPE_SUCCESS;
		memcpy(addr, vf_mac, ETH_ALEN);
	} else {
		msgbuf[0] |= IXGBE_VT_MSGTYPE_FAILURE;
	}

	/*
	 * Piggyback the multicast filter type so VF can compute the
	 * correct vectors
	 */
	msgbuf[3] = hw->mac.mc_filter_type;
	ixgbe_write_mbx(hw, msgbuf, IXGBE_VF_PERMADDR_MSG_LEN, vf);

	return 0;
}

static int ixgbe_set_vf_mac_addr(struct ixgbe_adapter *adapter,
				 u32 *msgbuf, u32 vf)
{
	u8 *new_mac = ((u8 *)(&msgbuf[1]));

	if (!is_valid_ether_addr(new_mac)) {
		e_warn(drv, "VF %d attempted to set invalid mac\n", vf);
		return -1;
	}

	if (adapter->vfinfo[vf].pf_set_mac && !adapter->vfinfo[vf].trusted &&
	    memcmp(adapter->vfinfo[vf].vf_mac_addresses, new_mac,
		   ETH_ALEN)) {
		u8 *pm = adapter->vfinfo[vf].vf_mac_addresses;
		e_warn(drv,
		       "VF %d attempted to set a new MAC address but it already has an administratively set MAC address %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X\n",
		       vf, pm[0], pm[1], pm[2], pm[3], pm[4], pm[5]);
		e_warn(drv, "Check the VF driver and if it is not using the correct MAC address you may need to reload the VF driver\n");
		return -1;
	}
	return ixgbe_set_vf_mac(adapter, vf, new_mac) < 0;
}

static int ixgbe_set_vf_vlan_msg(struct ixgbe_adapter *adapter,
				 u32 *msgbuf, u32 vf)
{
	u32 add = (msgbuf[0] & IXGBE_VT_MSGINFO_MASK) >> IXGBE_VT_MSGINFO_SHIFT;
	u32 vid = (msgbuf[1] & IXGBE_VLVF_VLANID_MASK);
	u8 tcs = netdev_get_num_tc(adapter->netdev);
	int err = 0;

	if (adapter->vfinfo[vf].pf_vlan || tcs) {
		e_warn(drv,
		       "VF %d attempted to override administratively set VLAN configuration\n"
		       "Reload the VF driver to resume operations\n",
		       vf);
		return -1;
	}

	/* VLAN 0 is a special case, don't allow it to be removed */
	if (!vid && !add)
		return 0;

	err = ixgbe_set_vf_vlan(adapter, add, vid, vf);
	if (err)
		goto out;

#ifdef HAVE_VLAN_RX_REGISTER
	/* in case of promiscuous mode any VLAN filter set for a VF must
	 * also have the PF pool added to it.
	 */
	if (add && adapter->netdev->flags & IFF_PROMISC) {
		err = ixgbe_set_vf_vlan(adapter, add, vid, VMDQ_P(0));
		if (err)
			goto out;
	}

#ifdef CONFIG_PCI_IOV
	/* Go through all the checks to see if the VLAN filter should
	 * be wiped completely.
	 */
	if (!add && adapter->netdev->flags & IFF_PROMISC) {
		struct ixgbe_hw *hw = &adapter->hw;
		u32 bits, vlvf;
		s32 reg_ndx;

		reg_ndx = ixgbe_find_vlvf_entry(hw, vid);
		if (reg_ndx < 0)
			goto out;
		vlvf = IXGBE_READ_REG(hw, IXGBE_VLVF(reg_ndx));
		/* See if any other pools are set for this VLAN filter
		 * entry other than the PF.
		 */
		if (VMDQ_P(0) < 32) {
			bits = IXGBE_READ_REG(hw, IXGBE_VLVFB(reg_ndx * 2));
			bits &= ~(1 << VMDQ_P(0));
			bits |= IXGBE_READ_REG(hw,
					       IXGBE_VLVFB(reg_ndx * 2) + 1);
		} else {
			bits = IXGBE_READ_REG(hw,
					      IXGBE_VLVFB(reg_ndx * 2) + 1);
			bits &= ~(1 << (VMDQ_P(0) - 32));
			bits |= IXGBE_READ_REG(hw, IXGBE_VLVFB(reg_ndx * 2));
		}

		/* If the filter was removed then ensure PF pool bit
		 * is cleared if the PF only added itself to the pool
		 * because the PF is in promiscuous mode.
		 */
		if ((vlvf & VLAN_VID_MASK) == vid && !bits)
			err = ixgbe_set_vf_vlan(adapter, add, vid, VMDQ_P(0));
	}

#endif /* CONFIG_PCI_IOV */
#endif /* HAVE_VLAN_RX_REGISTER */
out:
	return err;
}

static int ixgbe_set_vf_macvlan_msg(struct ixgbe_adapter *adapter,
				    u32 *msgbuf, u32 vf)
{
	u8 *new_mac = ((u8 *)(&msgbuf[1]));
	int index = (msgbuf[0] & IXGBE_VT_MSGINFO_MASK) >>
		    IXGBE_VT_MSGINFO_SHIFT;
	int err;

	if (adapter->vfinfo[vf].pf_set_mac && !adapter->vfinfo[vf].trusted &&
	    index > 0) {
		e_warn(drv,
		       "VF %d requested MACVLAN filter but is administratively denied\n",
		       vf);
		return -1;
	}

	/* An non-zero index indicates the VF is setting a filter */
	if (index) {
		if (!is_valid_ether_addr(new_mac)) {
			e_warn(drv, "VF %d attempted to set invalid mac\n", vf);
			return -1;
		}

		/*
		 * If the VF is allowed to set MAC filters then turn off
		 * anti-spoofing to avoid false positives.
		 */
		if (adapter->vfinfo[vf].spoofchk_enabled) {
			struct ixgbe_hw *hw = &adapter->hw;

			hw->mac.ops.set_mac_anti_spoofing(hw, false, vf);
			hw->mac.ops.set_vlan_anti_spoofing(hw, false, vf);
		}
	}

	err = ixgbe_set_vf_macvlan(adapter, vf, index, new_mac);
	if (err == -ENOSPC)
		e_warn(drv,
		       "VF %d has requested a MACVLAN filter but there is no space for it\n",
		       vf);

	return err < 0;
}

static int ixgbe_negotiate_vf_api(struct ixgbe_adapter *adapter,
				  u32 *msgbuf, u32 vf)
{
	int api = msgbuf[1];

	switch (api) {
	case ixgbe_mbox_api_10:
	case ixgbe_mbox_api_11:
	case ixgbe_mbox_api_12:
	case ixgbe_mbox_api_13:
		adapter->vfinfo[vf].vf_api = api;
		return 0;
	default:
		break;
	}

	e_info(drv, "VF %d requested invalid api version %u\n", vf, api);

	return -1;
}

static int ixgbe_get_vf_queues(struct ixgbe_adapter *adapter,
			       u32 *msgbuf, u32 vf)
{
	struct net_device *dev = adapter->netdev;
	struct ixgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	unsigned int default_tc = 0;
	u8 num_tcs = netdev_get_num_tc(dev);

	/* verify the PF is supporting the correct APIs */
	switch (adapter->vfinfo[vf].vf_api) {
	case ixgbe_mbox_api_20:
	case ixgbe_mbox_api_11:
	case ixgbe_mbox_api_12:
	case ixgbe_mbox_api_13:
		break;
	default:
		return -1;
	}

	/* only allow 1 Tx queue for bandwidth limiting */
	msgbuf[IXGBE_VF_TX_QUEUES] = __ALIGN_MASK(1, ~vmdq->mask);
	msgbuf[IXGBE_VF_RX_QUEUES] = __ALIGN_MASK(1, ~vmdq->mask);

	/* if TCs > 1 determine which TC belongs to default user priority */
	if (num_tcs > 1)
		default_tc = netdev_get_prio_tc_map(dev, adapter->default_up);

	/* notify VF of need for VLAN tag stripping, and correct queue */
	if (num_tcs)
		msgbuf[IXGBE_VF_TRANS_VLAN] = num_tcs;
	else if (adapter->vfinfo[vf].pf_vlan || adapter->vfinfo[vf].pf_qos)
		msgbuf[IXGBE_VF_TRANS_VLAN] = 1;
	else
		msgbuf[IXGBE_VF_TRANS_VLAN] = 0;

	/* notify VF of default queue */
	msgbuf[IXGBE_VF_DEF_QUEUE] = default_tc;

	return 0;
}

#ifdef HAVE_NDO_SET_VF_RSS_QUERY_EN
static int ixgbe_get_vf_reta(struct ixgbe_adapter *adapter, u32 *msgbuf, u32 vf)
{
	u32 i, j;
	u32 *out_buf = &msgbuf[1];
	const u8 *reta = adapter->rss_indir_tbl;
	u32 reta_size = ixgbe_rss_indir_tbl_entries(adapter);

	/* Check if operation is permitted */
	if (!adapter->vfinfo[vf].rss_query_enabled)
		return -EPERM;

	/* verify the PF is supporting the correct API */
	switch (adapter->vfinfo[vf].vf_api) {
	case ixgbe_mbox_api_12:
	case ixgbe_mbox_api_13:
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* This mailbox command is supported (required) only for 82599 and x540
	 * VFs which support up to 4 RSS queues. Therefore we will compress the
	 * RETA by saving only 2 bits from each entry. This way we will be able
	 * to transfer the whole RETA in a single mailbox operation.
	 */
	for (i = 0; i < reta_size / 16; i++) {
		out_buf[i] = 0;
		for (j = 0; j < 16; j++)
			out_buf[i] |= (u32)(reta[16 * i + j] & 0x3) << (2 * j);
	}

	return 0;
}

static int ixgbe_get_vf_rss_key(struct ixgbe_adapter *adapter,
				u32 *msgbuf, u32 vf)
{
	u32 *rss_key = &msgbuf[1];

	/* Check if the operation is permitted */
	if (!adapter->vfinfo[vf].rss_query_enabled)
		return -EPERM;

	/* verify the PF is supporting the correct API */
	switch (adapter->vfinfo[vf].vf_api) {
	case ixgbe_mbox_api_12:
	case ixgbe_mbox_api_13:
		break;
	default:
		return -EOPNOTSUPP;
	}

	memcpy(rss_key, adapter->rss_key, IXGBE_RSS_KEY_SIZE);

	return 0;
}
#endif /* HAVE_NDO_SET_VF_RSS_QUERY_EN */

static int ixgbe_update_vf_xcast_mode(struct ixgbe_adapter *adapter,
				      u32 *msgbuf, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	int xcast_mode = msgbuf[1];
	u32 vmolr, fctrl, disable, enable;

	/* verify the PF is supporting the correct APIs */
	switch (adapter->vfinfo[vf].vf_api) {
	case ixgbe_mbox_api_12:
		/* promisc introduced in 1.3 version */
		if (xcast_mode == IXGBEVF_XCAST_MODE_PROMISC)
			return -EOPNOTSUPP;
		/* Fall threw */
	case ixgbe_mbox_api_13:
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (xcast_mode > IXGBEVF_XCAST_MODE_MULTI &&
	    !adapter->vfinfo[vf].trusted) {
		xcast_mode = IXGBEVF_XCAST_MODE_MULTI;
	}

	if (adapter->vfinfo[vf].xcast_mode == xcast_mode)
		goto out;

	switch (xcast_mode) {
	case IXGBEVF_XCAST_MODE_NONE:
		disable = IXGBE_VMOLR_BAM | IXGBE_VMOLR_ROMPE |
			  IXGBE_VMOLR_MPE | IXGBE_VMOLR_UPE | IXGBE_VMOLR_VPE;
		enable = 0;
		break;
	case IXGBEVF_XCAST_MODE_MULTI:
		disable = IXGBE_VMOLR_MPE | IXGBE_VMOLR_UPE | IXGBE_VMOLR_VPE;
		enable = IXGBE_VMOLR_BAM | IXGBE_VMOLR_ROMPE;
		break;
	case IXGBEVF_XCAST_MODE_ALLMULTI:
		disable = IXGBE_VMOLR_UPE | IXGBE_VMOLR_VPE;
		enable = IXGBE_VMOLR_BAM | IXGBE_VMOLR_ROMPE | IXGBE_VMOLR_MPE;
		break;
	case IXGBEVF_XCAST_MODE_PROMISC:
		if (hw->mac.type <= ixgbe_mac_82599EB)
			return -EOPNOTSUPP;

		fctrl = IXGBE_READ_REG(hw, IXGBE_FCTRL);
		if (!(fctrl & IXGBE_FCTRL_UPE)) {
			/* VF promisc requires PF in promisc */
			e_warn(drv,
			       "Enabling VF promisc requires PF in promisc\n");
			return -EPERM;
		}

		disable = 0;
		enable = IXGBE_VMOLR_BAM | IXGBE_VMOLR_ROMPE |
			 IXGBE_VMOLR_MPE | IXGBE_VMOLR_UPE | IXGBE_VMOLR_VPE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	vmolr = IXGBE_READ_REG(hw, IXGBE_VMOLR(vf));
	vmolr &= ~disable;
	vmolr |= enable;
	IXGBE_WRITE_REG(hw, IXGBE_VMOLR(vf), vmolr);

	adapter->vfinfo[vf].xcast_mode = xcast_mode;

out:
	msgbuf[1] = xcast_mode;

	return 0;
}

static int ixgbe_get_vf_link_state(struct ixgbe_adapter *adapter,
				   u32 *msgbuf, u32 vf)
{
	u32 *link_state = &msgbuf[1];

	/* verify the PF is supporting the correct API */
	switch (adapter->vfinfo[vf].vf_api) {
	case ixgbe_mbox_api_12:
	case ixgbe_mbox_api_13:
		break;
	default:
		return -EOPNOTSUPP;
	}

	*link_state = adapter->vfinfo[vf].link_enable;

	return 0;
}

static int ixgbe_rcv_msg_from_vf(struct ixgbe_adapter *adapter, u32 vf)
{
	u32 mbx_size = IXGBE_VFMAILBOX_SIZE;
	u32 msgbuf[IXGBE_VFMAILBOX_SIZE];
	struct ixgbe_hw *hw = &adapter->hw;
	s32 retval;

	retval = ixgbe_read_mbx(hw, msgbuf, mbx_size, vf);

	if (retval) {
		pr_err("Error receiving message from VF\n");
		return retval;
	}

	/* this is a message we already processed, do nothing */
	if (msgbuf[0] & (IXGBE_VT_MSGTYPE_SUCCESS | IXGBE_VT_MSGTYPE_FAILURE))
		return retval;

	/* flush the ack before we write any messages back */
	IXGBE_WRITE_FLUSH(hw);

	if (msgbuf[0] == IXGBE_VF_RESET)
		return ixgbe_vf_reset_msg(adapter, vf);

	/*
	 * until the vf completes a virtual function reset it should not be
	 * allowed to start any configuration.
	 */

	if (!adapter->vfinfo[vf].clear_to_send) {
		msgbuf[0] |= IXGBE_VT_MSGTYPE_FAILURE;
		ixgbe_write_mbx(hw, msgbuf, 1, vf);
		return retval;
	}

	switch ((msgbuf[0] & 0xFFFF)) {
	case IXGBE_VF_SET_MAC_ADDR:
		retval = ixgbe_set_vf_mac_addr(adapter, msgbuf, vf);
		break;
	case IXGBE_VF_SET_MULTICAST:
		retval = ixgbe_set_vf_multicasts(adapter, msgbuf, vf);
		break;
	case IXGBE_VF_SET_VLAN:
		retval = ixgbe_set_vf_vlan_msg(adapter, msgbuf, vf);
		break;
	case IXGBE_VF_SET_LPE:
		if (msgbuf[1] > IXGBE_MAX_JUMBO_FRAME_SIZE) {
			e_err(drv, "VF max_frame %d out of range\n", msgbuf[1]);
			return -EINVAL;
		}
		retval = ixgbe_set_vf_lpe(adapter, msgbuf[1], vf);
		break;
	case IXGBE_VF_SET_MACVLAN:
		retval = ixgbe_set_vf_macvlan_msg(adapter, msgbuf, vf);
		break;
	case IXGBE_VF_API_NEGOTIATE:
		retval = ixgbe_negotiate_vf_api(adapter, msgbuf, vf);
		break;
	case IXGBE_VF_GET_QUEUES:
		retval = ixgbe_get_vf_queues(adapter, msgbuf, vf);
		break;
#ifdef HAVE_NDO_SET_VF_RSS_QUERY_EN
	case IXGBE_VF_GET_RETA:
		retval = ixgbe_get_vf_reta(adapter, msgbuf, vf);
		break;
	case IXGBE_VF_GET_RSS_KEY:
		retval = ixgbe_get_vf_rss_key(adapter, msgbuf, vf);
		break;
#endif /* HAVE_NDO_SET_VF_RSS_QUERY_EN */
	case IXGBE_VF_UPDATE_XCAST_MODE:
		retval = ixgbe_update_vf_xcast_mode(adapter, msgbuf, vf);
		break;
	case IXGBE_VF_GET_LINK_STATE:
		retval = ixgbe_get_vf_link_state(adapter, msgbuf, vf);
		break;
	default:
		e_err(drv, "Unhandled Msg %8.8x\n", msgbuf[0]);
		retval = IXGBE_ERR_MBX;
		break;
	}

	/* notify the VF of the results of what it sent us */
	if (retval)
		msgbuf[0] |= IXGBE_VT_MSGTYPE_FAILURE;
	else
		msgbuf[0] |= IXGBE_VT_MSGTYPE_SUCCESS;

	msgbuf[0] |= IXGBE_VT_MSGTYPE_CTS;

	ixgbe_write_mbx(hw, msgbuf, mbx_size, vf);

	return retval;
}

static void ixgbe_rcv_ack_from_vf(struct ixgbe_adapter *adapter, u32 vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 msg = IXGBE_VT_MSGTYPE_FAILURE;

	/* if device isn't clear to send it shouldn't be reading either */
	if (!adapter->vfinfo[vf].clear_to_send)
		ixgbe_write_mbx(hw, &msg, 1, vf);
}

#define Q_BITMAP_DEPTH 2
int ixgbe_check_mdd_event(struct ixgbe_adapter *adapter)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 vf_bitmap[Q_BITMAP_DEPTH] = { 0 };
	int retval = false;
	u32 j, i;
	u32 ping;

	if (!hw->mac.ops.mdd_event)
		return retval;

	/* Did we have a malicious event */
	hw->mac.ops.mdd_event(hw, vf_bitmap);

	/* Log any blocked queues and release lock */
	for (i = 0; i < Q_BITMAP_DEPTH; i++) {
		for (j = 0; j < 32 && vf_bitmap[i]; j++) {
			u32 vf;

			if (!(vf_bitmap[i] & (1 << j)))
				continue;

			/* The VF that malicious event occurred on */
			vf = j + (i * 32);

			dev_warn(ixgbe_pf_to_dev(adapter),
				 "Malicious event on VF %d tx:%x rx:%x\n", vf,
				 IXGBE_READ_REG(hw, IXGBE_LVMMC_TX),
				 IXGBE_READ_REG(hw, IXGBE_LVMMC_RX));

			/* restart the vf */
			if (hw->mac.ops.restore_mdd_vf) {
				hw->mac.ops.restore_mdd_vf(hw, vf);

				/* get the VF to rebuild its queues */
				adapter->vfinfo[vf].clear_to_send = 0;
				ping = IXGBE_PF_CONTROL_MSG |
				       IXGBE_VT_MSGTYPE_CTS;
				ixgbe_write_mbx(hw, &ping, 1, vf);
			}

			retval = true;
		}
	}

	return retval;
}

void ixgbe_msg_task(struct ixgbe_adapter *adapter)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 vf;

	if (!adapter->vfinfo)
		return;
	if (adapter->flags & IXGBE_FLAG_MDD_ENABLED)
		ixgbe_check_mdd_event(adapter);

	for (vf = 0; vf < adapter->num_vfs; vf++) {
		/* process any reset requests */
		if (!ixgbe_check_for_rst(hw, vf))
			ixgbe_vf_reset_event(adapter, vf);

		/* process any messages pending */
		if (!ixgbe_check_for_msg(hw, vf))
			ixgbe_rcv_msg_from_vf(adapter, vf);

		/* process any acks */
		if (!ixgbe_check_for_ack(hw, vf))
			ixgbe_rcv_ack_from_vf(adapter, vf);
	}
}

#if defined(HAVE_NDO_SET_VF_TRUST) || defined(HAVE_NDO_SET_VF_LINK_STATE)
static inline void ixgbe_ping_vf(struct ixgbe_adapter *adapter, int vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 ping;

	ping = IXGBE_PF_CONTROL_MSG;
	if (adapter->vfinfo[vf].clear_to_send)
		ping |= IXGBE_VT_MSGTYPE_CTS;
	ixgbe_write_mbx(hw, &ping, 1, vf);
}
#endif

void ixgbe_ping_all_vfs(struct ixgbe_adapter *adapter)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 ping;
	int i;

	for (i = 0 ; i < adapter->num_vfs; i++) {
		ping = IXGBE_PF_CONTROL_MSG;
		if (adapter->vfinfo[i].clear_to_send)
			ping |= IXGBE_VT_MSGTYPE_CTS;
		ixgbe_write_mbx(hw, &ping, 1, i);
	}
}

/**
 * ixgbe_set_all_vfs - update vfs queues
 * @adapter: Pointer to adapter struct
 *
 * Update setting transmit and receive queues for all vfs
 **/
void ixgbe_set_all_vfs(struct ixgbe_adapter *adapter)
{
	int i;

	for (i = 0 ; i < adapter->num_vfs; i++) {
		ixgbe_set_vf_link_state(adapter, i,
					adapter->vfinfo[i].link_state);
	}
}

#ifdef HAVE_NDO_SET_VF_TRUST
int ixgbe_ndo_set_vf_trust(struct net_device *netdev, int vf, bool setting)
{
	struct ixgbe_adapter *adapter = netdev_priv(netdev);

	if (vf < 0 || vf >= adapter->num_vfs)
		return -EINVAL;

	/* nothing to do */
	if (adapter->vfinfo[vf].trusted == setting)
		return 0;

	adapter->vfinfo[vf].trusted = setting;

	/* reset VF to reconfigure features */
	adapter->vfinfo[vf].clear_to_send = false;
	ixgbe_ping_vf(adapter, vf);

	e_info(drv, "VF %u is %strusted\n", vf, setting ? "" : "not ");

	return 0;
}

#endif
#ifdef IFLA_VF_MAX
int ixgbe_ndo_set_vf_mac(struct net_device *netdev, int vf, u8 *mac)
{
	struct ixgbe_adapter *adapter = netdev_priv(netdev);
	s32 retval = 0;

	if (vf < 0 || vf >= adapter->num_vfs)
		return -EINVAL;

	if (is_valid_ether_addr(mac)) {
		dev_info(ixgbe_pf_to_dev(adapter), "setting MAC %pM on VF %d\n",
			 mac, vf);
		dev_info(ixgbe_pf_to_dev(adapter), "Reload the VF driver to make this change effective.\n");

		retval = ixgbe_set_vf_mac(adapter, vf, mac);
		if (retval >= 0) {
			adapter->vfinfo[vf].pf_set_mac = true;

			if (test_bit(__IXGBE_DOWN, adapter->state)) {
				dev_warn(ixgbe_pf_to_dev(adapter), "The VF MAC address has been set, but the PF device is not up.\n");
				dev_warn(ixgbe_pf_to_dev(adapter), "Bring the PF device up before attempting to use the VF device.\n");
			}
		} else {
			dev_warn(ixgbe_pf_to_dev(adapter), "The VF MAC address was NOT set due to invalid or duplicate MAC address.\n");
		}
	} else if (is_zero_ether_addr(mac)) {
		unsigned char *vf_mac_addr =
					   adapter->vfinfo[vf].vf_mac_addresses;

		/* nothing to do */
		if (is_zero_ether_addr(vf_mac_addr))
			return 0;

		dev_info(ixgbe_pf_to_dev(adapter), "removing MAC on VF %d\n",
			 vf);

		retval = ixgbe_del_mac_filter(adapter, vf_mac_addr, vf);
		if (retval >= 0) {
			adapter->vfinfo[vf].pf_set_mac = false;
			memcpy(vf_mac_addr, mac, ETH_ALEN);
		} else {
			dev_warn(ixgbe_pf_to_dev(adapter), "Could NOT remove the VF MAC address.\n");
		}
	} else {
		retval = -EINVAL;
	}
	return retval;
}

static int ixgbe_enable_port_vlan(struct ixgbe_adapter *adapter,
				   int vf, u16 vlan, u8 qos)
{
	struct ixgbe_hw *hw = &adapter->hw;
	int err;

	err = ixgbe_set_vf_vlan(adapter, true, vlan, vf);
	if (err)
		goto out;

	/* Revoke tagless access via VLAN 0 */
	ixgbe_set_vf_vlan(adapter, false, 0, vf);

	ixgbe_set_vmvir(adapter, vlan, qos, vf);
	ixgbe_set_vmolr(hw, vf, false);

	/* enable hide vlan on X550 */
	if (hw->mac.type >= ixgbe_mac_X550)
		ixgbe_write_qde(adapter, vf, IXGBE_QDE_ENABLE |
				IXGBE_QDE_HIDE_VLAN);
	adapter->vfinfo[vf].pf_vlan = vlan;
	adapter->vfinfo[vf].pf_qos = qos;
	dev_info(ixgbe_pf_to_dev(adapter),
		 "Setting VLAN %d, QOS 0x%x on VF %d\n", vlan, qos, vf);
	if (test_bit(__IXGBE_DOWN, adapter->state)) {
		dev_warn(ixgbe_pf_to_dev(adapter), "The VF VLAN has been set, but the PF device is not up.\n");
		dev_warn(ixgbe_pf_to_dev(adapter), "Bring the PF device up before attempting to use the VF device.\n");
	}

out:
	return err;
}

static int ixgbe_disable_port_vlan(struct ixgbe_adapter *adapter, int vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	int err;

	err = ixgbe_set_vf_vlan(adapter, false,
				adapter->vfinfo[vf].pf_vlan, vf);
	/* Restore tagless access via VLAN 0 */
	ixgbe_set_vf_vlan(adapter, true, 0, vf);
	ixgbe_clear_vmvir(adapter, vf);
	ixgbe_set_vmolr(hw, vf, true);

	/* disable hide VLAN on X550 */
	if (hw->mac.type >= ixgbe_mac_X550)
		ixgbe_write_qde(adapter, vf, IXGBE_QDE_ENABLE);

	adapter->vfinfo[vf].pf_vlan = 0;
	adapter->vfinfo[vf].pf_qos = 0;

	return err;
}

#ifdef IFLA_VF_VLAN_INFO_MAX
int ixgbe_ndo_set_vf_vlan(struct net_device *netdev, int vf, u16 vlan,
			  u8 qos, __be16 vlan_proto)
#else
int ixgbe_ndo_set_vf_vlan(struct net_device *netdev, int vf, u16 vlan, u8 qos)
#endif
{
	int err = 0;
	struct ixgbe_adapter *adapter = netdev_priv(netdev);

	/* VLAN IDs accepted range 0-4094 */
	if (vf < 0 || vf >= adapter->num_vfs ||
	    vlan > VLAN_VID_MASK - 1 || qos > 7)
		return -EINVAL;

#ifdef IFLA_VF_VLAN_INFO_MAX
	if (vlan_proto != htons(ETH_P_8021Q))
		return -EPROTONOSUPPORT;
#endif
	if (vlan || qos) {
		/*
		 * Check if there is already a port VLAN set, if so
		 * we have to delete the old one first before we
		 * can set the new one.  The usage model had
		 * previously assumed the user would delete the
		 * old port VLAN before setting a new one but this
		 * is not necessarily the case.
		 */
		if (adapter->vfinfo[vf].pf_vlan)
			err = ixgbe_disable_port_vlan(adapter, vf);
		if (err)
			goto out;
		err = ixgbe_enable_port_vlan(adapter, vf, vlan, qos);

	} else {
		err = ixgbe_disable_port_vlan(adapter, vf);
	}
out:
	return err;
}


static int ixgbe_link_mbps(struct ixgbe_adapter *adapter)
{
	switch (adapter->link_speed) {
	case IXGBE_LINK_SPEED_100_FULL:
		return 100;
	case IXGBE_LINK_SPEED_1GB_FULL:
		return 1000;
	case IXGBE_LINK_SPEED_10GB_FULL:
		return 10000;
	default:
		return 0;
	}
}

static void ixgbe_set_vf_rate_limit(struct ixgbe_adapter *adapter, int vf)
{
	struct ixgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	struct ixgbe_hw *hw = &adapter->hw;
	u32 bcnrc_val = 0;
	u16 queue, queues_per_pool;
	u16 tx_rate = adapter->vfinfo[vf].tx_rate;

	if (tx_rate) {
		/* start with base link speed value */
		bcnrc_val = adapter->vf_rate_link_speed;

		/* Calculate the rate factor values to set */
		bcnrc_val <<= IXGBE_RTTBCNRC_RF_INT_SHIFT;
		bcnrc_val /= tx_rate;

		/* clear everything but the rate factor */
		bcnrc_val &= IXGBE_RTTBCNRC_RF_INT_MASK |
			     IXGBE_RTTBCNRC_RF_DEC_MASK;

		/* enable the rate scheduler */
		bcnrc_val |= IXGBE_RTTBCNRC_RS_ENA;
	}

	/*
	 * Set global transmit compensation time to the MMW_SIZE in RTTBCNRM
	 * register. Typically MMW_SIZE=0x014 if 9728-byte jumbo is supported
	 * and 0x004 otherwise.
	 */
	switch (hw->mac.type) {
	case ixgbe_mac_82599EB:
		IXGBE_WRITE_REG(hw, IXGBE_RTTBCNRM, 0x4);
		break;
	case ixgbe_mac_X540:
		IXGBE_WRITE_REG(hw, IXGBE_RTTBCNRM, 0x14);
		break;
	default:
		break;
	}

	/* determine how many queues per pool based on VMDq mask */
	queues_per_pool = __ALIGN_MASK(1, ~vmdq->mask);

	/* write value for all Tx queues belonging to VF */
	for (queue = 0; queue < queues_per_pool; queue++) {
		unsigned int reg_idx = (vf * queues_per_pool) + queue;

		IXGBE_WRITE_REG(hw, IXGBE_RTTDQSEL, reg_idx);
		IXGBE_WRITE_REG(hw, IXGBE_RTTBCNRC, bcnrc_val);
	}
}

void ixgbe_check_vf_rate_limit(struct ixgbe_adapter *adapter)
{
	int i;

	/* VF Tx rate limit was not set */
	if (!adapter->vf_rate_link_speed)
		return;

	if (ixgbe_link_mbps(adapter) != adapter->vf_rate_link_speed) {
		adapter->vf_rate_link_speed = 0;
		dev_info(ixgbe_pf_to_dev(adapter),
			 "Link speed has been changed. VF Transmit rate is disabled\n");
	}

	for (i = 0; i < adapter->num_vfs; i++) {
		if (!adapter->vf_rate_link_speed)
			adapter->vfinfo[i].tx_rate = 0;

		ixgbe_set_vf_rate_limit(adapter, i);
	}
}

#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
int ixgbe_ndo_set_vf_bw(struct net_device *netdev,
			int vf,
			int __always_unused min_tx_rate,
			int max_tx_rate)
#else
int ixgbe_ndo_set_vf_bw(struct net_device *netdev, int vf, int max_tx_rate)
#endif /* HAVE_NDO_SET_VF_MIN_MAX_TX_RATE */
{
	struct ixgbe_adapter *adapter = netdev_priv(netdev);
	int link_speed;

	/* verify VF is active */
	if (vf < 0 || vf >= adapter->num_vfs)
		return -EINVAL;

	/* verify link is up */
	if (!adapter->link_up)
		return -EINVAL;

	/* verify we are linked at 10Gbps */
	link_speed = ixgbe_link_mbps(adapter);
	if (link_speed != 10000)
		return -EINVAL;

	/* rate limit cannot be less than 10Mbs or greater than link speed */
	if (max_tx_rate && ((max_tx_rate <= 10) || (max_tx_rate > link_speed)))
		return -EINVAL;

	/* store values */
	adapter->vf_rate_link_speed = link_speed;
	adapter->vfinfo[vf].tx_rate = max_tx_rate;

	/* update hardware configuration */
	ixgbe_set_vf_rate_limit(adapter, vf);

	return 0;
}

#endif /* IFLA_VF_MAX */
#if IS_ENABLED(CONFIG_PCI_IOV)
int ixgbe_ndo_set_vf_spoofchk(struct net_device *netdev, int vf, bool setting)
{
	struct ixgbe_adapter *adapter = netdev_priv(netdev);
	struct ixgbe_hw *hw = &adapter->hw;

	if (vf < 0 || vf >= adapter->num_vfs)
		return -EINVAL;

	adapter->vfinfo[vf].spoofchk_enabled = setting;

	/* configure MAC spoofing */
	hw->mac.ops.set_mac_anti_spoofing(hw, setting, vf);

	/* configure VLAN spoofing */
	hw->mac.ops.set_vlan_anti_spoofing(hw, setting, vf);

	/* Ensure LLDP and FC is set for Ethertype Antispoofing if we will be
	 * calling set_ethertype_anti_spoofing for each VF in loop below
	 */
	if (hw->mac.ops.set_ethertype_anti_spoofing) {
		IXGBE_WRITE_REG(hw, IXGBE_ETQF(IXGBE_ETQF_FILTER_LLDP),
				(IXGBE_ETQF_FILTER_EN    |
				 IXGBE_ETQF_TX_ANTISPOOF |
				 IXGBE_ETH_P_LLDP));

		IXGBE_WRITE_REG(hw, IXGBE_ETQF(IXGBE_ETQF_FILTER_FC),
				(IXGBE_ETQF_FILTER_EN |
				 IXGBE_ETQF_TX_ANTISPOOF |
				 ETH_P_PAUSE));

		hw->mac.ops.set_ethertype_anti_spoofing(hw, setting, vf);
	}
	return 0;
}

#endif /* CONFIG_PCI_IOV */

/**
 * ixgbe_set_vf_rx_tx - Set VF rx tx
 * @adapter: Pointer to adapter struct
 * @vf: VF identifier
 *
 * Set or reset correct transmit and receive for vf
 **/
static void ixgbe_set_vf_rx_tx(struct ixgbe_adapter *adapter, int vf)
{
	struct ixgbe_hw *hw = &adapter->hw;
	u32 reg_cur_tx, reg_cur_rx, reg_req_tx, reg_req_rx;
	u32 reg_offset, vf_shift;

	vf_shift = vf % 32;
	reg_offset = vf / 32;

	reg_cur_tx = IXGBE_READ_REG(hw, IXGBE_VFTE(reg_offset));
	reg_cur_rx = IXGBE_READ_REG(hw, IXGBE_VFRE(reg_offset));

	if (adapter->vfinfo[vf].link_enable) {
		reg_req_tx = reg_cur_tx | 1 << vf_shift;
		reg_req_rx = reg_cur_rx | 1 << vf_shift;

	} else {
		reg_req_tx = reg_cur_tx & ~(1 << vf_shift);
		reg_req_rx = reg_cur_rx & ~(1 << vf_shift);
	}

	/*
	 * The 82599 cannot support a mix of jumbo and non-jumbo PF/VFs.
	 * For more info take a look at ixgbe_set_vf_lpe
	 */
	if (adapter->hw.mac.type == ixgbe_mac_82599EB) {
		struct net_device *dev = adapter->netdev;
		int pf_max_frame = dev->mtu + ETH_HLEN;

#if IS_ENABLED(CONFIG_FCOE)
		if (dev->features & NETIF_F_FCOE_MTU)
			pf_max_frame = max_t(int, pf_max_frame,
					     IXGBE_FCOE_JUMBO_FRAME_SIZE);
#endif /* CONFIG_FCOE */

		if (pf_max_frame > ETH_FRAME_LEN)
			reg_req_rx = reg_cur_rx & ~(1 << vf_shift);
	}

	/* Enable/Disable particular VF */
	if (reg_cur_tx != reg_req_tx)
		IXGBE_WRITE_REG(hw, IXGBE_VFTE(reg_offset), reg_req_tx);
	if (reg_cur_rx != reg_req_rx)
		IXGBE_WRITE_REG(hw, IXGBE_VFRE(reg_offset), reg_req_rx);
}

#ifdef HAVE_NDO_SET_VF_LINK_STATE

/**
 * ixgbe_set_vf_link_state - Set link state
 * @adapter: Pointer to adapter struct
 * @vf: VF identifier
 * @state: required link state
 *
 * Set a link force state on/off a single vf
 **/
void ixgbe_set_vf_link_state(struct ixgbe_adapter *adapter, int vf, int state)
{
	adapter->vfinfo[vf].link_state = state;

	switch (state) {
	case IFLA_VF_LINK_STATE_AUTO:
		if (test_bit(__IXGBE_DOWN, adapter->state))
			adapter->vfinfo[vf].link_enable = false;
		else
			adapter->vfinfo[vf].link_enable = true;
		break;
	case IFLA_VF_LINK_STATE_ENABLE:
		adapter->vfinfo[vf].link_enable = true;
		break;
	case IFLA_VF_LINK_STATE_DISABLE:
		adapter->vfinfo[vf].link_enable = false;
		break;
	}

	ixgbe_set_vf_rx_tx(adapter, vf);

	/* restart the VF */
	adapter->vfinfo[vf].clear_to_send = false;
	ixgbe_ping_vf(adapter, vf);
}

/**
 * ixgbe_ndo_set_vf_link_state - Set link state
 * @netdev: network interface device structure
 * @vf: VF identifier
 * @state: required link state
 *
 * Set the link state of a specified VF, regardless of physical link state
 **/
int ixgbe_ndo_set_vf_link_state(struct net_device *netdev, int vf, int state)
{
	struct ixgbe_adapter *adapter = netdev_priv(netdev);
	int ret = 0;

	if (vf < 0 || vf >= adapter->num_vfs) {
		dev_err(ixgbe_pf_to_dev(adapter),
			"NDO set VF link - invalid VF identifier %d\n", vf);
		ret = -EINVAL;
		goto out;
	}

	switch (state) {
	case IFLA_VF_LINK_STATE_ENABLE:
		dev_info(ixgbe_pf_to_dev(adapter),
			 "NDO set VF %d link state %d - not supported\n",
			vf, state);
		break;
	case IFLA_VF_LINK_STATE_DISABLE:
		dev_info(ixgbe_pf_to_dev(adapter),
			 "NDO set VF %d link state disable\n", vf);
		ixgbe_set_vf_link_state(adapter, vf, state);
		break;
	case IFLA_VF_LINK_STATE_AUTO:
		dev_info(ixgbe_pf_to_dev(adapter),
			 "NDO set VF %d link state auto\n", vf);
		ixgbe_set_vf_link_state(adapter, vf, state);
		break;
	default:
		dev_err(ixgbe_pf_to_dev(adapter),
			"NDO set VF %d - invalid link state %d\n", vf, state);
		ret = -EINVAL;
	}
out:
	return ret;
}
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

#ifdef IFLA_VF_MAX
#ifdef HAVE_NDO_SET_VF_RSS_QUERY_EN
int ixgbe_ndo_set_vf_rss_query_en(struct net_device *netdev, int vf,
				  bool setting)
{
	struct ixgbe_adapter *adapter = netdev_priv(netdev);

	/* This operation is currently supported only for 82599 and x540
	 * devices.
	 */
	if (adapter->hw.mac.type < ixgbe_mac_82599EB ||
	    adapter->hw.mac.type >= ixgbe_mac_X550)
		return -EOPNOTSUPP;

	if (vf < 0 || vf >= adapter->num_vfs)
		return -EINVAL;

	adapter->vfinfo[vf].rss_query_enabled = setting;

	return 0;
}

#endif
int ixgbe_ndo_get_vf_config(struct net_device *netdev,
			    int vf, struct ifla_vf_info *ivi)
{
	struct ixgbe_adapter *adapter = netdev_priv(netdev);

	if (vf < 0 || vf >= adapter->num_vfs)
		return -EINVAL;

	ivi->vf = vf;
	memcpy(&ivi->mac, adapter->vfinfo[vf].vf_mac_addresses, ETH_ALEN);

#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
	ivi->max_tx_rate = adapter->vfinfo[vf].tx_rate;
	ivi->min_tx_rate = 0;
#else
	ivi->tx_rate = adapter->vfinfo[vf].tx_rate;
#endif /* HAVE_NDO_SET_VF_MIN_MAX_TX_RATE */

	ivi->vlan = adapter->vfinfo[vf].pf_vlan;
	ivi->qos = adapter->vfinfo[vf].pf_qos;

#ifdef HAVE_NDO_SET_VF_LINK_STATE
	ivi->linkstate = adapter->vfinfo[vf].link_state;
#endif
#ifdef HAVE_VF_SPOOFCHK_CONFIGURE
	ivi->spoofchk = adapter->vfinfo[vf].spoofchk_enabled;
#endif
#ifdef HAVE_NDO_SET_VF_RSS_QUERY_EN
	ivi->rss_query_en = adapter->vfinfo[vf].rss_query_enabled;
#endif
#ifdef HAVE_NDO_SET_VF_TRUST
	ivi->trusted = adapter->vfinfo[vf].trusted;
#endif
	return 0;
}
#endif /* IFLA_VF_MAX */

