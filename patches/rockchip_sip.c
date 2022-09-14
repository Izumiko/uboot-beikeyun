/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2016, Fuzhou Rockchip Electronics Co., Ltd
 */

#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/rockchip/rockchip_sip.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>
#include <uapi/linux/psci.h>
#include <linux/ptrace.h>

#ifdef CONFIG_64BIT
#define PSCI_FN_NATIVE(version, name)	PSCI_##version##_FN64_##name
#else
#define PSCI_FN_NATIVE(version, name)	PSCI_##version##_FN_##name
#endif

#define SIZE_PAGE(n)	((n) << 12)

static struct arm_smccc_res __invoke_sip_fn_smc(unsigned long function_id,
						unsigned long arg0,
						unsigned long arg1,
						unsigned long arg2)
{
	struct arm_smccc_res res;

	arm_smccc_smc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);
	return res;
}

struct arm_smccc_res sip_smc_ddr_cfg(u32 arg0, u32 arg1, u32 arg2)
{
	return __invoke_sip_fn_smc(SIP_DDR_CFG, arg0, arg1, arg2);
}

struct arm_smccc_res sip_smc_get_atf_version(void)
{
	return __invoke_sip_fn_smc(SIP_ATF_VERSION, 0, 0, 0);
}

struct arm_smccc_res sip_smc_get_sip_version(void)
{
	return __invoke_sip_fn_smc(SIP_SIP_VERSION, 0, 0, 0);
}

int sip_smc_set_suspend_mode(u32 ctrl, u32 config1, u32 config2)
{
	struct arm_smccc_res res;

	res = __invoke_sip_fn_smc(SIP_SUSPEND_MODE, ctrl, config1, config2);
	return res.a0;
}

int sip_smc_virtual_poweroff(void)
{
	struct arm_smccc_res res;

	res = __invoke_sip_fn_smc(PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND), 0, 0, 0);
	return res.a0;
}

struct arm_smccc_res sip_smc_request_share_mem(u32 page_num,
					       share_page_type_t page_type)
{
	struct arm_smccc_res res;
	unsigned long share_mem_phy;

	res = __invoke_sip_fn_smc(SIP_SHARE_MEM, page_num, page_type, 0);
	if (IS_SIP_ERROR(res.a0))
		goto error;

	share_mem_phy = res.a1;
	res.a1 = (unsigned long)ioremap(share_mem_phy, SIZE_PAGE(page_num));

error:
	return res;
}

struct arm_smccc_res sip_smc_mcu_el3fiq(u32 arg0, u32 arg1, u32 arg2)
{
	return __invoke_sip_fn_smc(SIP_MCU_EL3FIQ_CFG, arg0, arg1, arg2);
}

/************************** fiq debugger **************************************/
#ifdef CONFIG_ARM64
#define SIP_UARTDBG_FN		SIP_UARTDBG_CFG64
#else
#define SIP_UARTDBG_FN		SIP_UARTDBG_CFG
#endif

static int fiq_sip_enabled;
static int fiq_target_cpu;
static phys_addr_t ft_fiq_mem_phy;
static void __iomem *ft_fiq_mem_base;
static void (*sip_fiq_debugger_uart_irq_tf)(struct pt_regs _pt_regs,
					    unsigned long cpu);
int sip_fiq_debugger_is_enabled(void)
{
	return fiq_sip_enabled;
}

static struct pt_regs sip_fiq_debugger_get_pt_regs(void *reg_base,
						   unsigned long sp_el1)
{
	struct pt_regs fiq_pt_regs;

#ifdef CONFIG_ARM64
	/* copy cpu context */
	memcpy(&fiq_pt_regs, reg_base, 8 * 31);

	/* copy pstate */
	memcpy(&fiq_pt_regs.pstate, reg_base + 0x110, 8);

	/* EL1 mode */
	if (fiq_pt_regs.pstate & 0x10)
		memcpy(&fiq_pt_regs.sp, reg_base + 0xf8, 8);
	/* EL0 mode */
	else
		fiq_pt_regs.sp = sp_el1;

	/* copy pc */
	memcpy(&fiq_pt_regs.pc, reg_base + 0x118, 8);
#else
	struct sm_nsec_ctx *nsec_ctx = reg_base;

	fiq_pt_regs.ARM_r0 = nsec_ctx->r0;
	fiq_pt_regs.ARM_r1 = nsec_ctx->r1;
	fiq_pt_regs.ARM_r2 = nsec_ctx->r2;
	fiq_pt_regs.ARM_r3 = nsec_ctx->r3;
	fiq_pt_regs.ARM_r4 = nsec_ctx->r4;
	fiq_pt_regs.ARM_r5 = nsec_ctx->r5;
	fiq_pt_regs.ARM_r6 = nsec_ctx->r6;
	fiq_pt_regs.ARM_r7 = nsec_ctx->r7;
	fiq_pt_regs.ARM_r8 = nsec_ctx->r8;
	fiq_pt_regs.ARM_r9 = nsec_ctx->r9;
	fiq_pt_regs.ARM_r10 = nsec_ctx->r10;
	fiq_pt_regs.ARM_fp = nsec_ctx->r11;
	fiq_pt_regs.ARM_ip = nsec_ctx->r12;
	fiq_pt_regs.ARM_sp = nsec_ctx->svc_sp;
	fiq_pt_regs.ARM_lr = nsec_ctx->svc_lr;
	fiq_pt_regs.ARM_pc = nsec_ctx->mon_lr;
	fiq_pt_regs.ARM_cpsr = nsec_ctx->mon_spsr;
#endif

	return fiq_pt_regs;
}

static void sip_fiq_debugger_uart_irq_tf_cb(unsigned long sp_el1,
					    unsigned long offset,
					    unsigned long cpu)
{
	struct pt_regs fiq_pt_regs;
	char *cpu_context;

	/* calling fiq handler */
	if (ft_fiq_mem_base) {
		cpu_context = (char *)ft_fiq_mem_base + offset;
		fiq_pt_regs = sip_fiq_debugger_get_pt_regs(cpu_context, sp_el1);
		sip_fiq_debugger_uart_irq_tf(fiq_pt_regs, cpu);
	}

	/* fiq handler done, return to EL3(then EL3 return to EL1 entry) */
	__invoke_sip_fn_smc(SIP_UARTDBG_FN, 0, 0, UARTDBG_CFG_OSHDL_TO_OS);
}

int sip_fiq_debugger_uart_irq_tf_init(u32 irq_id, void *callback_fn)
{
	struct arm_smccc_res res;

	fiq_target_cpu = 0;

	/* init fiq debugger callback */
	sip_fiq_debugger_uart_irq_tf = callback_fn;
	res = __invoke_sip_fn_smc(SIP_UARTDBG_FN, irq_id,
				  (unsigned long)sip_fiq_debugger_uart_irq_tf_cb,
				  UARTDBG_CFG_INIT);
	if (IS_SIP_ERROR(res.a0)) {
		pr_err("%s error: %d\n", __func__, (int)res.a0);
		return res.a0;
	}

	/* share memory ioremap */
	if (!ft_fiq_mem_base) {
		ft_fiq_mem_phy = res.a1;
		ft_fiq_mem_base = ioremap(ft_fiq_mem_phy,
					  FIQ_UARTDBG_SHARE_MEM_SIZE);
		if (!ft_fiq_mem_base) {
			pr_err("%s: share memory ioremap failed\n", __func__);
			return -ENOMEM;
		}
	}

	fiq_sip_enabled = 1;

	return SIP_RET_SUCCESS;
}

int sip_fiq_debugger_switch_cpu(u32 cpu)
{
	struct arm_smccc_res res;

	fiq_target_cpu = cpu;
	res = __invoke_sip_fn_smc(SIP_UARTDBG_FN, cpu_logical_map(cpu),
				  0, UARTDBG_CFG_OSHDL_CPUSW);
	return res.a0;
}

void sip_fiq_debugger_enable_debug(bool enable)
{
	unsigned long val;

	val = enable ? UARTDBG_CFG_OSHDL_DEBUG_ENABLE :
		       UARTDBG_CFG_OSHDL_DEBUG_DISABLE;

	__invoke_sip_fn_smc(SIP_UARTDBG_FN, 0, 0, val);
}

int sip_fiq_debugger_set_print_port(u32 port_phyaddr, u32 baudrate)
{
	struct arm_smccc_res res;

	res = __invoke_sip_fn_smc(SIP_UARTDBG_FN, port_phyaddr, baudrate,
				  UARTDBG_CFG_PRINT_PORT);
	return res.a0;
}

int sip_fiq_debugger_request_share_memory(void)
{
	struct arm_smccc_res res;

	/* request page share memory */
	res = sip_smc_request_share_mem(FIQ_UARTDBG_PAGE_NUMS,
					SHARE_PAGE_TYPE_UARTDBG);
	if (IS_SIP_ERROR(res.a0))
		return res.a0;

	return SIP_RET_SUCCESS;
}

int sip_fiq_debugger_get_target_cpu(void)
{
	return fiq_target_cpu;
}

void sip_fiq_debugger_enable_fiq(bool enable, uint32_t tgt_cpu)
{
	u32 en;

	fiq_target_cpu = tgt_cpu;
	en = enable ? UARTDBG_CFG_FIQ_ENABEL : UARTDBG_CFG_FIQ_DISABEL;
	__invoke_sip_fn_smc(SIP_UARTDBG_FN, tgt_cpu, 0, en);
}
 