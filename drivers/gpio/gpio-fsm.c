// SPDX-License-Identifier: GPL-2.0+
/*
 *  GPIO FSM driver
 *
 *  This driver implements simple state machines that allow real GPIOs to be
 *  controlled in response to inputs from other GPIOs - real and soft/virtual -
 *  and time delays. It can:
 *  + create dummy GPIOs for drivers that demand them
 *  + drive multiple GPIOs from a single input,  with optional delays
 *  + add a debounce circuit to an input
 *  + drive pattern sequences onto LEDs
 *  etc.
 *
 *  Copyright (C) 2020 Raspberry Pi (Trading) Ltd.
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/gpio/gpio-fsm.h>

#define MODULE_NAME "gpio-fsm"

#define GF_IO_TYPE(x) ((u32)(x) & 0xffff)
#define GF_IO_INDEX(x) ((u32)(x) >> 16)

enum {
	SIGNAL_GPIO,
	SIGNAL_SOFT
};

enum {
	INPUT_GPIO,
	INPUT_SOFT
};

enum {
	SYM_UNDEFINED,
	SYM_NAME,
	SYM_SET,
	SYM_START,
	SYM_SHUTDOWN,

	SYM_MAX
};

struct soft_gpio {
	int dir;
	int value;
};

struct input_gpio_state {
	struct gpio_fsm *gf;
	struct gpio_desc  *desc;
	struct fsm_state *target;
	int index;
	int value;
	int irq;
	bool enabled;
	bool active_low;
};

struct gpio_event {
	int index;
	int value;
	struct fsm_state *target;
};

struct symtab_entry {
	const char *name;
	void *value;
	struct symtab_entry *next;
};

struct output_signal {
	u8 type;
	u8 value;
	u16 index;
};

struct fsm_state {
	const char *name;
	struct output_signal *signals;
	struct gpio_event *gpio_events;
	struct gpio_event *soft_events;
	struct fsm_state *delay_target;
	struct fsm_state *shutdown_target;
	unsigned int num_signals;
	unsigned int num_gpio_events;
	unsigned int num_soft_events;
	unsigned int delay_ms;
	unsigned int shutdown_ms;
};

struct gpio_fsm {
	struct gpio_chip gc;
	struct device *dev;
	spinlock_t spinlock;
	struct work_struct work;
	struct timer_list timer;
	wait_queue_head_t shutdown_event;
	struct fsm_state *states;
	struct input_gpio_state *input_gpio_states;
	struct gpio_descs *input_gpios;
	struct gpio_descs *output_gpios;
	struct soft_gpio *soft_gpios;
	struct fsm_state *start_state;
	struct fsm_state *shutdown_state;
	unsigned int num_states;
	unsigned int num_output_gpios;
	unsigned int num_input_gpios;
	unsigned int num_soft_gpios;
	unsigned int shutdown_timeout_ms;
	unsigned int shutdown_jiffies;

	struct fsm_state *current_state;
	struct fsm_state *next_state;
	struct fsm_state *delay_target_state;
	int delay_ms;
	unsigned int debug;
	bool shutting_down;
	struct symtab_entry *symtab;
};

static struct symtab_entry *do_add_symbol(struct symtab_entry **symtab,
					  const char *name, void *value)
{
	struct symtab_entry **p = symtab;

	while (*p && strcmp((*p)->name, name))
		p = &(*p)->next;

	if (*p) {
		/* This is an existing symbol */
		if ((*p)->value) {
			/* Already defined */
			if (value) {
				if ((uintptr_t)value < SYM_MAX)
					return ERR_PTR(-EINVAL);
				else
					return ERR_PTR(-EEXIST);
			}
		} else {
			/* Undefined */
			(*p)->value = value;
		}
	} else {
		/* This is a new symbol */
		*p = kmalloc(sizeof(struct symtab_entry), GFP_KERNEL);
		if (*p) {
			(*p)->name = name;
			(*p)->value = value;
			(*p)->next = NULL;
		}
	}
	return *p;
}

static int add_symbol(struct symtab_entry **symtab,
		      const char *name, void *value)
{
	struct symtab_entry *sym = do_add_symbol(symtab, name, value);

	return PTR_ERR_OR_ZERO(sym);
}

static struct symtab_entry *get_symbol(struct symtab_entry **symtab,
				       const char *name)
{
	struct symtab_entry *sym = do_add_symbol(symtab, name, NULL);

	if (IS_ERR(sym))
		return NULL;
	return sym;
}

static void free_symbols(struct symtab_entry **symtab)
{
	struct symtab_entry *sym = *symtab;
	void *p;

	*symtab = NULL;
	while (sym) {
		p = sym;
		sym = sym->next;
		kfree(p);
	}
}

static int gpio_fsm_get_direction(struct gpio_chip *gc, unsigned int off)
{
	struct gpio_fsm *gf = gpiochip_get_data(gc);
	struct soft_gpio *sg;

	if (off >= gf->num_soft_gpios)
		return -EINVAL;
	sg = &gf->soft_gpios[off];

	return sg->dir;
}

static int gpio_fsm_get(struct gpio_chip *gc, unsigned int off)
{
	struct gpio_fsm *gf = gpiochip_get_data(gc);
	struct soft_gpio *sg;

	if (off >= gf->num_soft_gpios)
		return -EINVAL;
	sg = &gf->soft_gpios[off];

	return sg->value;
}

static void gpio_fsm_go_to_state(struct gpio_fsm *gf,
				   struct fsm_state *new_state)
{
	struct input_gpio_state *inp_state;
	struct gpio_event *gp_ev;
	struct fsm_state *state;
	int i;

	dev_dbg(gf->dev, "go_to_state(%s)\n",
		  new_state ? new_state->name : "<unset>");

	spin_lock(&gf->spinlock);

	if (gf->next_state) {
		/* Something else has already requested a transition */
		spin_unlock(&gf->spinlock);
		return;
	}

	gf->next_state = new_state;
	state = gf->current_state;
	gf->delay_target_state = NULL;

	if (state) {
		/* Disarm any GPIO IRQs */
		for (i = 0; i < state->num_gpio_events; i++) {
			gp_ev = &state->gpio_events[i];
			inp_state = &gf->input_gpio_states[gp_ev->index];
			inp_state->target = NULL;
		}
	}

	spin_unlock(&gf->spinlock);

	if (new_state)
		schedule_work(&gf->work);
}

static void gpio_fsm_set_soft(struct gpio_fsm *gf,
				unsigned int off, int val)
{
	struct soft_gpio *sg = &gf->soft_gpios[off];
	struct gpio_event *gp_ev;
	struct fsm_state *state;
	int i;

	dev_dbg(gf->dev, "set(%d,%d)\n", off, val);
	state = gf->current_state;
	sg->value = val;
	for (i = 0; i < state->num_soft_events; i++) {
		gp_ev = &state->soft_events[i];
		if (gp_ev->index == off && gp_ev->value == val) {
			if (gf->debug)
				dev_info(gf->dev,
					 "GF_SOFT %d->%d -> %s\n", gp_ev->index,
					 gp_ev->value, gp_ev->target->name);
			gpio_fsm_go_to_state(gf, gp_ev->target);
			break;
		}
	}
}

static int gpio_fsm_direction_input(struct gpio_chip *gc, unsigned int off)
{
	struct gpio_fsm *gf = gpiochip_get_data(gc);
	struct soft_gpio *sg;

	if (off >= gf->num_soft_gpios)
		return -EINVAL;
	sg = &gf->soft_gpios[off];
	sg->dir = GPIOF_DIR_IN;

	return 0;
}

static int gpio_fsm_direction_output(struct gpio_chip *gc, unsigned int off,
				       int value)
{
	struct gpio_fsm *gf = gpiochip_get_data(gc);
	struct soft_gpio *sg;

	if (off >= gf->num_soft_gpios)
		return -EINVAL;
	sg = &gf->soft_gpios[off];
	sg->dir = GPIOF_DIR_OUT;
	gpio_fsm_set_soft(gf, off, value);

	return 0;
}

static void gpio_fsm_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct gpio_fsm *gf;

	gf = gpiochip_get_data(gc);
	if (off < gf->num_soft_gpios)
		gpio_fsm_set_soft(gf, off, val);
}

static void gpio_fsm_enter_state(struct gpio_fsm *gf,
				   struct fsm_state *state)
{
	struct input_gpio_state *inp_state;
	struct output_signal *signal;
	struct gpio_event *event;
	struct gpio_desc *gpiod;
	struct soft_gpio *soft;
	int value;
	int i;

	dev_dbg(gf->dev, "enter_state(%s)\n", state->name);

	gf->current_state = state;

	// 1. Apply any listed signals
	for (i = 0; i < state->num_signals; i++) {
		signal = &state->signals[i];

		if (gf->debug)
			dev_info(gf->dev, "  set %s %d->%d\n",
				 (signal->type == SIGNAL_GPIO) ? "GF_OUT" :
				 "GF_SOFT",
				 signal->index, signal->value);
		switch (signal->type) {
		case SIGNAL_GPIO:
			gpiod = gf->output_gpios->desc[signal->index];
			gpiod_set_value_cansleep(gpiod, signal->value);
			break;
		case SIGNAL_SOFT:
			soft = &gf->soft_gpios[signal->index];
			gpio_fsm_set_soft(gf, signal->index, signal->value);
			break;
		}
	}

	// 2. Exit if successfully reached shutdown state
	if (gf->shutting_down && state == state->shutdown_target) {
		wake_up(&gf->shutdown_event);
		return;
	}

	// 3. Schedule a timer callback if shutting down
	if (state->shutdown_target) {
		// Remember the absolute shutdown time in case remove is called
		// at a later time.
		gf->shutdown_jiffies =
			jiffies + msecs_to_jiffies(state->shutdown_ms);

		if (gf->shutting_down) {
			gf->delay_target_state = state->shutdown_target;
			gf->delay_ms = state->shutdown_ms;
			mod_timer(&gf->timer, gf->shutdown_jiffies);
		}
	}

	// During shutdown, skip everything else
	if (gf->shutting_down)
		return;

	// Otherwise record what the shutdown time would be
	gf->shutdown_jiffies = jiffies + msecs_to_jiffies(state->shutdown_ms);

	// 4. Check soft inputs for transitions to take
	for (i = 0; i < state->num_soft_events; i++) {
		event = &state->soft_events[i];
		if (gf->soft_gpios[event->index].value == event->value) {
			if (gf->debug)
				dev_info(gf->dev,
					 "GF_SOFT %d=%d -> %s\n", event->index,
					 event->value, event->target->name);
			gpio_fsm_go_to_state(gf, event->target);
			return;
		}
	}

	// 5. Check GPIOs for transitions to take, enabling the IRQs
	for (i = 0; i < state->num_gpio_events; i++) {
		event = &state->gpio_events[i];
		inp_state = &gf->input_gpio_states[event->index];
		inp_state->target = event->target;
		inp_state->value = event->value;
		inp_state->enabled = true;

		value = gpiod_get_value(gf->input_gpios->desc[event->index]);

		// Clear stale event state
		disable_irq(inp_state->irq);

		irq_set_irq_type(inp_state->irq,
				 (inp_state->value ^ inp_state->active_low) ?
				 IRQF_TRIGGER_RISING : IRQF_TRIGGER_FALLING);
		enable_irq(inp_state->irq);

		if (value == event->value && inp_state->target) {
			if (gf->debug)
				dev_info(gf->dev,
					 "GF_IN %d=%d -> %s\n", event->index,
					 event->value, event->target->name);
			gpio_fsm_go_to_state(gf, event->target);
			return;
		}
	}

	// 6. Schedule a timer callback if delay_target
	if (state->delay_target) {
		gf->delay_target_state = state->delay_target;
		gf->delay_ms = state->delay_ms;
		mod_timer(&gf->timer,
			  jiffies + msecs_to_jiffies(state->delay_ms));
	}
}

static void gpio_fsm_work(struct work_struct *work)
{
	struct input_gpio_state *inp_state;
	struct fsm_state *new_state;
	struct fsm_state *state;
	struct gpio_event *gp_ev;
	struct gpio_fsm *gf;
	int i;

	gf = container_of(work, struct gpio_fsm, work);
	spin_lock(&gf->spinlock);
	state = gf->current_state;
	new_state = gf->next_state;
	if (!new_state)
		new_state = gf->delay_target_state;
	gf->next_state = NULL;
	gf->delay_target_state = NULL;
	spin_unlock(&gf->spinlock);

	if (state) {
		/* Disable any enabled GPIO IRQs */
		for (i = 0; i < state->num_gpio_events; i++) {
			gp_ev = &state->gpio_events[i];
			inp_state = &gf->input_gpio_states[gp_ev->index];
			if (inp_state->enabled) {
				inp_state->enabled = false;
				irq_set_irq_type(inp_state->irq,
						 IRQF_TRIGGER_NONE);
			}
		}
	}

	if (new_state)
		gpio_fsm_enter_state(gf, new_state);
}

static irqreturn_t gpio_fsm_gpio_irq_handler(int irq, void *dev_id)
{
	struct input_gpio_state *inp_state = dev_id;
	struct gpio_fsm *gf = inp_state->gf;
	struct fsm_state *target;

	target = inp_state->target;
	if (!target)
		return IRQ_NONE;

	/* If the IRQ has fired then the desired state _must_ have occurred */
	inp_state->enabled = false;
	irq_set_irq_type(inp_state->irq, IRQF_TRIGGER_NONE);
	if (gf->debug)
		dev_info(gf->dev, "GF_IN %d->%d -> %s\n",
			 inp_state->index, inp_state->value, target->name);
	gpio_fsm_go_to_state(gf, target);
	return IRQ_HANDLED;
}

static void gpio_fsm_timer(struct timer_list *timer)
{
	struct gpio_fsm *gf = container_of(timer, struct gpio_fsm, timer);
	struct fsm_state *target;

	target = gf->delay_target_state;
	if (!target)
		return;

	if (gf->debug)
		dev_info(gf->dev, "GF_DELAY %d -> %s\n", gf->delay_ms,
			 target->name);

	gpio_fsm_go_to_state(gf, target);
}

int gpio_fsm_parse_signals(struct gpio_fsm *gf, struct fsm_state *state,
			     struct property *prop)
{
	const __be32 *cells = prop->value;
	struct output_signal *signal;
	u32 io;
	u32 type;
	u32 index;
	u32 value;
	int ret = 0;
	int i;

	if (prop->length % 8) {
		dev_err(gf->dev, "malformed set in state %s\n",
			state->name);
		return -EINVAL;
	}

	state->num_signals = prop->length/8;
	state->signals = devm_kcalloc(gf->dev, state->num_signals,
				      sizeof(struct output_signal),
				      GFP_KERNEL);
	for (i = 0; i < state->num_signals; i++) {
		signal = &state->signals[i];
		io = be32_to_cpu(cells[0]);
		type = GF_IO_TYPE(io);
		index = GF_IO_INDEX(io);
		value = be32_to_cpu(cells[1]);

		if (type != GF_OUT && type != GF_SOFT) {
			dev_err(gf->dev,
				"invalid set type %d in state %s\n",
				type, state->name);
			ret = -EINVAL;
			break;
		}
		if (type == GF_OUT && index >= gf->num_output_gpios) {
			dev_err(gf->dev,
				"invalid GF_OUT number %d in state %s\n",
				index, state->name);
			ret = -EINVAL;
			break;
		}
		if (type == GF_SOFT && index >= gf->num_soft_gpios) {
			dev_err(gf->dev,
				"invalid GF_SOFT number %d in state %s\n",
				index, state->name);
			ret = -EINVAL;
			break;
		}
		if (value != 0 && value != 1) {
			dev_err(gf->dev,
				"invalid set value %d in state %s\n",
				value, state->name);
			ret = -EINVAL;
			break;
		}
		signal->type = (type == GF_OUT) ? SIGNAL_GPIO : SIGNAL_SOFT;
		signal->index = index;
		signal->value = value;
		cells += 2;
	}

	return ret;
}

struct gpio_event *new_event(struct gpio_event **events, int *num_events)
{
	int num = ++(*num_events);
	*events = krealloc(*events, num * sizeof(struct gpio_event),
			   GFP_KERNEL);
	return *events ? *events + (num - 1) : NULL;
}

int gpio_fsm_parse_events(struct gpio_fsm *gf, struct fsm_state *state,
			    struct property *prop)
{
	const __be32 *cells = prop->value;
	struct symtab_entry *sym;
	int num_cells;
	int ret = 0;
	int i;

	if (prop->length % 8) {
		dev_err(gf->dev,
			"malformed transitions from state %s to state %s\n",
			state->name, prop->name);
		return -EINVAL;
	}

	sym = get_symbol(&gf->symtab, prop->name);
	num_cells = prop->length / 4;
	i = 0;
	while (i < num_cells) {
		struct gpio_event *gp_ev;
		u32 event, param;
		u32 index;

		event = be32_to_cpu(cells[i++]);
		param = be32_to_cpu(cells[i++]);
		index = GF_IO_INDEX(event);

		switch (GF_IO_TYPE(event)) {
		case GF_IN:
			if (index >= gf->num_input_gpios) {
				dev_err(gf->dev,
					"invalid GF_IN %d in transitions from state %s to state %s\n",
					index, state->name, prop->name);
				return -EINVAL;
			}
			if (param > 1) {
				dev_err(gf->dev,
					"invalid GF_IN value %d in transitions from state %s to state %s\n",
					param, state->name, prop->name);
				return -EINVAL;
			}
			gp_ev = new_event(&state->gpio_events,
					  &state->num_gpio_events);
			if (!gp_ev)
				return -ENOMEM;
			gp_ev->index = index;
			gp_ev->value = param;
			gp_ev->target = (struct fsm_state *)sym;
			break;

		case GF_SOFT:
			if (index >= gf->num_soft_gpios) {
				dev_err(gf->dev,
					"invalid GF_SOFT %d in transitions from state %s to state %s\n",
					index, state->name, prop->name);
				return -EINVAL;
			}
			if (param > 1) {
				dev_err(gf->dev,
					"invalid GF_SOFT value %d in transitions from state %s to state %s\n",
					param, state->name, prop->name);
				return -EINVAL;
			}
			gp_ev = new_event(&state->soft_events,
					  &state->num_soft_events);
			if (!gp_ev)
				return -ENOMEM;
			gp_ev->index = index;
			gp_ev->value = param;
			gp_ev->target = (struct fsm_state *)sym;
			break;

		case GF_DELAY:
			if (state->delay_target) {
				dev_err(gf->dev,
					"state %s has multiple GF_DELAYs\n",
					state->name);
				return -EINVAL;
			}
			state->delay_target = (struct fsm_state *)sym;
			state->delay_ms = param;
			break;

		case GF_SHUTDOWN:
			if (state->shutdown_target == state) {
				dev_err(gf->dev,
					"shutdown state %s has GF_SHUTDOWN\n",
					state->name);
				return -EINVAL;
			} else if (state->shutdown_target) {
				dev_err(gf->dev,
					"state %s has multiple GF_SHUTDOWNs\n",
					state->name);
				return -EINVAL;
			}
			state->shutdown_target =
				(struct fsm_state *)sym;
			state->shutdown_ms = param;
			break;

		default:
			dev_err(gf->dev,
				"invalid event %08x in transitions from state %s to state %s\n",
				event, state->name, prop->name);
			return -EINVAL;
		}
	}
	if (i != num_cells) {
		dev_err(gf->dev,
			"malformed transitions from state %s to state %s\n",
			state->name, prop->name);
		return -EINVAL;
	}

	return ret;
}

int gpio_fsm_parse_state(struct gpio_fsm *gf,
			   struct fsm_state *state,
			   struct device_node *np)
{
	struct symtab_entry *sym;
	struct property *prop;
	int ret;

	state->name = np->name;
	ret = add_symbol(&gf->symtab, np->name, state);
	if (ret) {
		switch (ret) {
		case -EINVAL:
			dev_err(gf->dev, "'%s' is not a valid state name\n",
				np->name);
			break;
		case -EEXIST:
			dev_err(gf->dev, "state %s already defined\n",
				np->name);
			break;
		default:
			dev_err(gf->dev, "error %d adding state %s symbol\n",
				ret, np->name);
			break;
		}
		return ret;
	}

	for_each_property_of_node(np, prop) {
		sym = get_symbol(&gf->symtab, prop->name);
		if (!sym) {
			ret = -ENOMEM;
			break;
		}

		switch ((uintptr_t)sym->value) {
		case SYM_SET:
			ret = gpio_fsm_parse_signals(gf, state, prop);
			break;
		case SYM_START:
			if (gf->start_state) {
				dev_err(gf->dev, "multiple start states\n");
				ret = -EINVAL;
			} else {
				gf->start_state = state;
			}
			break;
		case SYM_SHUTDOWN:
			state->shutdown_target = state;
			gf->shutdown_state = state;
			break;
		case SYM_NAME:
			/* Ignore */
			break;
		default:
			/* A set of transition events to this state */
			ret = gpio_fsm_parse_events(gf, state, prop);
			break;
		}
	}

	return ret;
}

static void dump_all(struct gpio_fsm *gf)
{
	int i, j;

	dev_info(gf->dev, "Input GPIOs:\n");
	for (i = 0; i < gf->num_input_gpios; i++)
		dev_info(gf->dev, "  %d: %p\n", i,
			 gf->input_gpios->desc[i]);

	dev_info(gf->dev, "Output GPIOs:\n");
	for (i = 0; i < gf->num_output_gpios; i++)
		dev_info(gf->dev, "  %d: %p\n", i,
			 gf->output_gpios->desc[i]);

	dev_info(gf->dev, "Soft GPIOs:\n");
	for (i = 0; i < gf->num_soft_gpios; i++)
		dev_info(gf->dev, "  %d: %s %d\n", i,
			 (gf->soft_gpios[i].dir == GPIOF_DIR_IN) ? "IN" : "OUT",
			 gf->soft_gpios[i].value);

	dev_info(gf->dev, "Start state: %s\n",
		 gf->start_state ? gf->start_state->name : "-");

	dev_info(gf->dev, "Shutdown timeout: %d ms\n",
		 gf->shutdown_timeout_ms);

	for (i = 0; i < gf->num_states; i++) {
		struct fsm_state *state = &gf->states[i];

		dev_info(gf->dev, "State %s:\n", state->name);

		if (state->shutdown_target == state)
			dev_info(gf->dev, "  Shutdown state\n");

		dev_info(gf->dev, "  Signals:\n");
		for (j = 0; j < state->num_signals; j++) {
			struct output_signal *signal = &state->signals[j];

			dev_info(gf->dev, "    %d: %s %d=%d\n", j,
				 (signal->type == SIGNAL_GPIO) ? "GPIO" :
								 "SOFT",
				 signal->index, signal->value);
		}

		dev_info(gf->dev, "  GPIO events:\n");
		for (j = 0; j < state->num_gpio_events; j++) {
			struct gpio_event *event = &state->gpio_events[j];

			dev_info(gf->dev, "    %d: %d=%d -> %s\n", j,
				 event->index, event->value,
				 event->target->name);
		}

		dev_info(gf->dev, "  Soft events:\n");
		for (j = 0; j < state->num_soft_events; j++) {
			struct gpio_event *event = &state->soft_events[j];

			dev_info(gf->dev, "    %d: %d=%d -> %s\n", j,
				 event->index, event->value,
				 event->target->name);
		}

		if (state->delay_target)
			dev_info(gf->dev, "  Delay: %d ms -> %s\n",
				 state->delay_ms, state->delay_target->name);

		if (state->shutdown_target && state->shutdown_target != state)
			dev_info(gf->dev, "  Shutdown: %d ms -> %s\n",
				 state->shutdown_ms,
				 state->shutdown_target->name);
	}
	dev_info(gf->dev, "\n");
}

static int resolve_sym_to_state(struct gpio_fsm *gf, struct fsm_state **pstate)
{
	struct symtab_entry *sym = (struct symtab_entry *)*pstate;

	if (!sym)
		return -ENOMEM;

	*pstate = sym->value;

	if (!*pstate) {
		dev_err(gf->dev, "state %s not defined\n",
			sym->name);
		return -EINVAL;
	}

	return 0;
}

static int gpio_fsm_probe(struct platform_device *pdev)
{
	struct input_gpio_state *inp_state;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *cp;
	struct gpio_fsm *gf;
	u32 debug = 0;
	int num_states;
	u32 num_soft_gpios;
	int ret;
	int i;
	static const char *const reserved_symbols[] = {
		[SYM_NAME] = "name",
		[SYM_SET] = "set",
		[SYM_START] = "start_state",
		[SYM_SHUTDOWN] = "shutdown_state",
	};

	if (of_property_read_u32(np, "num-swgpios", &num_soft_gpios) &&
	    of_property_read_u32(np, "num-soft-gpios", &num_soft_gpios)) {
		dev_err(dev, "missing 'num-swgpios' property\n");
		return -EINVAL;
	}

	of_property_read_u32(np, "debug", &debug);

	gf = devm_kzalloc(dev, sizeof(*gf), GFP_KERNEL);
	if (!gf)
		return -ENOMEM;

	gf->dev = dev;
	gf->debug = debug;

	if (of_property_read_u32(np, "shutdown-timeout-ms",
				 &gf->shutdown_timeout_ms))
		gf->shutdown_timeout_ms = 5000;

	gf->num_soft_gpios = num_soft_gpios;
	gf->soft_gpios = devm_kcalloc(dev, num_soft_gpios,
				      sizeof(struct soft_gpio), GFP_KERNEL);
	if (!gf->soft_gpios)
		return -ENOMEM;
	for (i = 0; i < num_soft_gpios; i++) {
		struct soft_gpio *sg = &gf->soft_gpios[i];

		sg->dir = GPIOF_DIR_IN;
		sg->value = 0;
	}

	gf->input_gpios = devm_gpiod_get_array_optional(dev, "input", GPIOD_IN);
	if (IS_ERR(gf->input_gpios)) {
		ret = PTR_ERR(gf->input_gpios);
		dev_err(dev, "failed to get input gpios from DT - %d\n", ret);
		return ret;
	}
	gf->num_input_gpios = (gf->input_gpios ? gf->input_gpios->ndescs : 0);

	gf->input_gpio_states = devm_kcalloc(dev, gf->num_input_gpios,
					     sizeof(struct input_gpio_state),
					     GFP_KERNEL);
	if (!gf->input_gpio_states)
		return -ENOMEM;
	for (i = 0; i < gf->num_input_gpios; i++) {
		inp_state = &gf->input_gpio_states[i];
		inp_state->desc = gf->input_gpios->desc[i];
		inp_state->gf = gf;
		inp_state->index = i;
		inp_state->irq = gpiod_to_irq(inp_state->desc);
		inp_state->active_low = gpiod_is_active_low(inp_state->desc);
		if (inp_state->irq >= 0)
			ret = devm_request_irq(gf->dev, inp_state->irq,
					       gpio_fsm_gpio_irq_handler,
					       IRQF_TRIGGER_NONE,
					       dev_name(dev),
					       inp_state);
		else
			ret = inp_state->irq;

		if (ret) {
			dev_err(dev,
				"failed to get IRQ for input gpio - %d\n",
				ret);
			return ret;
		}
	}

	gf->output_gpios = devm_gpiod_get_array_optional(dev, "output",
							 GPIOD_OUT_LOW);
	if (IS_ERR(gf->output_gpios)) {
		ret = PTR_ERR(gf->output_gpios);
		dev_err(dev, "failed to get output gpios from DT - %d\n", ret);
		return ret;
	}
	gf->num_output_gpios = (gf->output_gpios ? gf->output_gpios->ndescs :
				0);

	num_states = of_get_child_count(np);
	if (!num_states) {
		dev_err(dev, "no states declared\n");
		return -EINVAL;
	}
	gf->states = devm_kcalloc(dev, num_states,
				  sizeof(struct fsm_state), GFP_KERNEL);
	if (!gf->states)
		return -ENOMEM;

	// add reserved words to the symbol table
	for (i = 0; i < ARRAY_SIZE(reserved_symbols); i++) {
		if (reserved_symbols[i])
			add_symbol(&gf->symtab, reserved_symbols[i],
				   (void *)(uintptr_t)i);
	}

	// parse the state
	for_each_child_of_node(np, cp) {
		struct fsm_state *state = &gf->states[gf->num_states];

		ret = gpio_fsm_parse_state(gf, state, cp);
		if (ret)
			return ret;
		gf->num_states++;
	}

	if (!gf->start_state) {
		dev_err(gf->dev, "no start state defined\n");
		return -EINVAL;
	}

	// resolve symbol pointers into state pointers
	for (i = 0; !ret && i < gf->num_states; i++) {
		struct fsm_state *state = &gf->states[i];
		int j;

		for (j = 0; !ret && j < state->num_gpio_events; j++) {
			struct gpio_event *ev = &state->gpio_events[j];

			ret = resolve_sym_to_state(gf, &ev->target);
		}

		for (j = 0; !ret && j < state->num_soft_events; j++) {
			struct gpio_event *ev = &state->soft_events[j];

			ret = resolve_sym_to_state(gf, &ev->target);
		}

		if (!ret) {
			resolve_sym_to_state(gf, &state->delay_target);
			if (state->shutdown_target != state)
				resolve_sym_to_state(gf,
						     &state->shutdown_target);
		}
	}

	if (!ret && gf->debug > 1)
		dump_all(gf);

	free_symbols(&gf->symtab);

	if (ret)
		return ret;

	gf->gc.parent = dev;
	gf->gc.label = np->name;
	gf->gc.owner = THIS_MODULE;
	gf->gc.of_node = np;
	gf->gc.base = -1;
	gf->gc.ngpio = num_soft_gpios;

	gf->gc.get_direction = gpio_fsm_get_direction;
	gf->gc.direction_input = gpio_fsm_direction_input;
	gf->gc.direction_output = gpio_fsm_direction_output;
	gf->gc.get = gpio_fsm_get;
	gf->gc.set = gpio_fsm_set;
	gf->gc.can_sleep = true;
	spin_lock_init(&gf->spinlock);
	INIT_WORK(&gf->work, gpio_fsm_work);
	timer_setup(&gf->timer, gpio_fsm_timer, 0);
	init_waitqueue_head(&gf->shutdown_event);

	platform_set_drvdata(pdev, gf);

	if (gf->debug)
		dev_info(gf->dev, "Start -> %s\n", gf->start_state->name);

	gpio_fsm_go_to_state(gf, gf->start_state);

	return devm_gpiochip_add_data(dev, &gf->gc, gf);
}

static int gpio_fsm_remove(struct platform_device *pdev)
{
	struct gpio_fsm *gf = platform_get_drvdata(pdev);
	int i;

	if (gf->shutdown_state) {
		if (gf->debug)
			dev_info(gf->dev, "Shutting down...\n");

		spin_lock(&gf->spinlock);
		gf->shutting_down = true;
		if (gf->current_state->shutdown_target &&
		    gf->current_state->shutdown_target != gf->current_state) {
			gf->delay_target_state =
				gf->current_state->shutdown_target;
			mod_timer(&gf->timer, gf->shutdown_jiffies);
		}
		spin_unlock(&gf->spinlock);

		wait_event_timeout(gf->shutdown_event,
				   gf->current_state->shutdown_target ==
				   gf->current_state,
				   msecs_to_jiffies(gf->shutdown_timeout_ms));
		if (gf->current_state->shutdown_target == gf->current_state)
			gpio_fsm_enter_state(gf, gf->shutdown_state);
	}
	cancel_work_sync(&gf->work);
	del_timer_sync(&gf->timer);

	/* Events aren't allocated from managed storage */
	for (i = 0; i < gf->num_states; i++) {
		kfree(gf->states[i].gpio_events);
		kfree(gf->states[i].soft_events);
	}
	if (gf->debug)
		dev_info(gf->dev, "Exiting\n");

	return 0;
}

static void gpio_fsm_shutdown(struct platform_device *pdev)
{
	gpio_fsm_remove(pdev);
}

static const struct of_device_id gpio_fsm_ids[] = {
	{ .compatible = "rpi,gpio-fsm" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_fsm_ids);

static struct platform_driver gpio_fsm_driver = {
	.driver	= {
		.name		= MODULE_NAME,
		.of_match_table	= of_match_ptr(gpio_fsm_ids),
	},
	.probe = gpio_fsm_probe,
	.remove = gpio_fsm_remove,
	.shutdown = gpio_fsm_shutdown,
};
module_platform_driver(gpio_fsm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Phil Elwell <phil@raspberrypi.com>");
MODULE_DESCRIPTION("GPIO FSM driver");
MODULE_ALIAS("platform:gpio-fsm");
