#ifndef __TAS256X_REGMAP__
#define __TAS256X_REGMAP__
#include <linux/version.h>

struct linux_platform {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct regmap *left_regmap;
	bool stopping;
	bool probe_ready;
	bool irq_requested[2];
	struct mutex irq_lock;
	struct hrtimer mtimer;
	struct snd_soc_component *codec;
	/* device is working, but system is suspended */
	int (*runtime_suspend)(struct tas256x_priv *p_tas256x);
	int (*runtime_resume)(struct tas256x_priv *p_tas256x);
	bool mb_runtime_suspend;
	bool i2c_suspend;
};

#endif /*__TAS256X_REGMAP__*/

