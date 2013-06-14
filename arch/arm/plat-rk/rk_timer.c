/*
 *
 * Copyright (C) 2013 ROCKCHIP, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/platform_device.h>

#include <asm/localtimer.h>
#include <asm/sched_clock.h>

#define TIMER_NAME "rk_timer"

#define TIMER_LOAD_COUNT0               0x00
#define TIMER_LOAD_COUNT1               0x04
#define TIMER_CURRENT_VALUE0            0x08
#define TIMER_CURRENT_VALUE1            0x0c
#define TIMER_CONTROL_REG               0x10
#define TIMER_INT_STATUS                0x18

#define TIMER_DISABLE                   (0 << 0)
#define TIMER_ENABLE                    (1 << 0)
#define TIMER_MODE_FREE_RUNNING         (0 << 1)
#define TIMER_MODE_USER_DEFINED_COUNT   (1 << 1)
#define TIMER_INT_MASK                  (0 << 2)
#define TIMER_INT_UNMASK                (1 << 2)

static inline void rk_timer_disable(void __iomem *base)
{
	writel_relaxed(TIMER_DISABLE, base + TIMER_CONTROL_REG);
	dsb();
}

static inline void rk_timer_enable(void __iomem *base, u32 flags)
{
	writel_relaxed(TIMER_ENABLE | flags, base + TIMER_CONTROL_REG);
	dsb();
}

static inline u32 rk_timer_read_current_value(void __iomem *base)
{
	return readl_relaxed(base + TIMER_CURRENT_VALUE0);
}

struct rk_timer {
	void __iomem *cs_base;
	struct clk *cs_clk;
	struct clk *cs_pclk;

	void __iomem *ce_base[NR_CPUS];
	struct irqaction ce_irq[NR_CPUS];
	bool ce_irq_disabled[NR_CPUS];
	struct clk *ce_clk[NR_CPUS];
	struct clk *ce_pclk[NR_CPUS];
	char ce_name[NR_CPUS][16];
};
static struct rk_timer timer;

static const char *platform_get_string_byname(struct platform_device *dev, const char *name)
{
	struct resource *r = platform_get_resource_byname(dev, 0, name);

	return r ? (const char *)r->start : NULL;
}

static int rk_timer_set_next_event(unsigned long cycles, struct clock_event_device *evt)
{
	unsigned int cpu = smp_processor_id();
	void __iomem *base = timer.ce_base[cpu];

	rk_timer_disable(base);
	writel_relaxed(cycles, base + TIMER_LOAD_COUNT0);
	writel_relaxed(0, base + TIMER_LOAD_COUNT1);
	dsb();
	rk_timer_enable(base, TIMER_MODE_USER_DEFINED_COUNT | TIMER_INT_UNMASK);
	return 0;
}

static void rk_timer_set_mode(enum clock_event_mode mode, struct clock_event_device *evt)
{
	unsigned int cpu = smp_processor_id();
	void __iomem *base = timer.ce_base[cpu];
	int irq = timer.ce_irq[cpu].irq;

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		rk_timer_disable(base);
		writel_relaxed(24000000 / HZ - 1, base + TIMER_LOAD_COUNT0);
		dsb();
		rk_timer_enable(base, TIMER_MODE_FREE_RUNNING | TIMER_INT_UNMASK);
	case CLOCK_EVT_MODE_RESUME:
	case CLOCK_EVT_MODE_ONESHOT:
		if (timer.ce_irq_disabled[cpu]) {
			enable_irq(irq);
			timer.ce_irq_disabled[cpu] = false;
		}
		break;
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
		rk_timer_disable(base);
		if (!timer.ce_irq_disabled[cpu]) {
			disable_irq(irq);
			timer.ce_irq_disabled[cpu] = true;
		}
		break;
	}
}

static irqreturn_t rk_timer_clockevent_interrupt(int irq, void *dev_id)
{
	unsigned int cpu = smp_processor_id();
	struct clock_event_device *evt = dev_id;
	void __iomem *base = timer.ce_base[cpu];

	/* clear interrupt */
	writel_relaxed(1, base + TIMER_INT_STATUS);
	if (evt->mode == CLOCK_EVT_MODE_ONESHOT) {
		writel_relaxed(TIMER_DISABLE, base + TIMER_CONTROL_REG);
	}
	dsb();

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static __cpuinit int rk_timer_init_clockevent(struct clock_event_device *ce, unsigned int cpu)
{
	struct irqaction *irq = &timer.ce_irq[cpu];
	void __iomem *base = timer.ce_base[cpu];

	ce->name = timer.ce_name[cpu];
	ce->features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	ce->set_next_event = rk_timer_set_next_event;
	ce->set_mode = rk_timer_set_mode;
	ce->irq = irq->irq;
	ce->cpumask = cpumask_of(cpu);

	writel_relaxed(1, base + TIMER_INT_STATUS);
	rk_timer_disable(base);

	irq->dev_id = ce;
	irq_set_affinity(irq->irq, cpumask_of(cpu));
	setup_irq(irq->irq, irq);

	clockevents_config_and_register(ce, 24000000, 0xF, 0xFFFFFFFF);

	return 0;
}

static cycle_t rk_timer_read(struct clocksource *cs)
{
	return ~rk_timer_read_current_value(timer.cs_base);
}

/*
 * Constants generated by clocksource_hz2mult(24000000, 26).
 * This gives a resolution of about 41ns and a wrap period of about 178s.
 */
#define MULT	2796202667u
#define SHIFT	26
#define MASK	(u32)~0

static struct clocksource rk_timer_clocksource = {
	.name           = TIMER_NAME,
	.rating         = 200,
	.read           = rk_timer_read,
	.mask           = CLOCKSOURCE_MASK(32),
	.flags          = CLOCK_SOURCE_IS_CONTINUOUS,
};

static void __init rk_timer_init_clocksource(void)
{
	static char err[] __initdata = KERN_ERR "%s: can't register clocksource!\n";
	struct clocksource *cs = &rk_timer_clocksource;
	void __iomem *base = timer.cs_base;

	clk_enable(timer.cs_pclk);
	clk_enable(timer.cs_clk);

	rk_timer_disable(base);
	writel_relaxed(0xFFFFFFFF, base + TIMER_LOAD_COUNT0);
	writel_relaxed(0xFFFFFFFF, base + TIMER_LOAD_COUNT1);
	dsb();
	rk_timer_enable(base, TIMER_MODE_FREE_RUNNING | TIMER_INT_MASK);

	if (clocksource_register_hz(cs, 24000000))
		printk(err, cs->name);
}

static DEFINE_CLOCK_DATA(cd);

unsigned long long notrace sched_clock(void)
{
	cycle_t cyc;

	if (!timer.cs_base)
		return 0;

	cyc = ~rk_timer_read_current_value(timer.cs_base);
	return cyc_to_fixed_sched_clock(&cd, cyc, MASK, MULT, SHIFT);
}

static void notrace rk_timer_update_sched_clock(void)
{
	u32 cyc = ~rk_timer_read_current_value(timer.cs_base);
	update_sched_clock(&cd, cyc, MASK);
}

static void __init rk_timer_init_sched_clock(void)
{
	init_fixed_sched_clock(&cd, rk_timer_update_sched_clock, 32, 24000000, MULT, SHIFT);
}

#ifndef CONFIG_LOCAL_TIMERS
static struct clock_event_device rk_timer_clockevent;
#endif

static int __init rk_timer_probe(struct platform_device *pdev)
{
	struct resource *res;
	int cpu;

	timer.cs_clk = clk_get(NULL, platform_get_string_byname(pdev, "cs_clk"));
	timer.cs_pclk = clk_get(NULL, platform_get_string_byname(pdev, "cs_pclk"));
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cs_base");
	timer.cs_base = (void *)res->start;

	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		char name[16];
		struct irqaction *irq = &timer.ce_irq[cpu];

		snprintf(timer.ce_name[cpu], sizeof(timer.ce_name[cpu]), TIMER_NAME "%d", cpu);

		snprintf(name, sizeof(name), "ce_clk%d", cpu);
		timer.ce_clk[cpu] = clk_get(NULL, platform_get_string_byname(pdev, name));

		snprintf(name, sizeof(name), "ce_pclk%d", cpu);
		timer.ce_pclk[cpu] = clk_get(NULL, platform_get_string_byname(pdev, name));

		snprintf(name, sizeof(name), "ce_base%d", cpu);
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
		timer.ce_base[cpu] = (void *)res->start;

		snprintf(name, sizeof(name), "ce_irq%d", cpu);
		irq->irq = platform_get_irq_byname(pdev, name);
		irq->name = timer.ce_name[cpu];
		irq->flags = IRQF_DISABLED | IRQF_TIMER | IRQF_NOBALANCING | IRQF_PERCPU;
		irq->handler = rk_timer_clockevent_interrupt;

		clk_enable(timer.ce_pclk[cpu]);
		clk_enable(timer.ce_clk[cpu]);
	}

	rk_timer_init_clocksource();
#ifndef CONFIG_LOCAL_TIMERS
	rk_timer_clockevent.rating = 200;
	rk_timer_init_clockevent(&rk_timer_clockevent, 0);
#endif
	rk_timer_init_sched_clock();

	printk("rk_timer: version 1.2\n");
	return 0;
}

static struct platform_driver rk_timer_driver __initdata = {
	.probe          = rk_timer_probe,
	.driver         = {
		.name   = TIMER_NAME,
	}
};

early_platform_init(TIMER_NAME, &rk_timer_driver);

#ifdef CONFIG_LOCAL_TIMERS
/*
 * Setup the local clock events for a CPU.
 */
static int __cpuinit rk_local_timer_setup(struct clock_event_device *clk)
{
	clk->rating = 450;
	return rk_timer_init_clockevent(clk, smp_processor_id());
}

/*
 * Setup the local clock events for a CPU.
 */
int __cpuinit local_timer_setup(struct clock_event_device *evt)
{
	return rk_local_timer_setup(evt);
}

/*
 * local_timer_ack: checks for a local timer interrupt.
 *
 * If a local timer interrupt has occurred, acknowledge and return 1.
 * Otherwise, return 0.
 */
int local_timer_ack(void)
{
	return 0;
}
#endif
