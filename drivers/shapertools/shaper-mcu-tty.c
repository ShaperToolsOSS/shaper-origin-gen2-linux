/*
 * Copyright 2020 Shaper Tools
 * Copyright 2019 NXP
 */

#define DEBUG 1

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/virtio.h>

/* this needs to be less then (RPMSG_BUF_SIZE - sizeof(struct rpmsg_hdr)) */
#define RPMSG_MAX_SIZE 256
#define MSG	"hello world!"

struct shaper_rpmsg_tty_port
{
	struct tty_port port;
	spinlock_t rx_lock;
	struct rpmsg_device *rpdev;
	struct tty_driver *rpmsg_tty_drv;
};

static int shaper_rpmsg_tty_cb(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src)
{
	int space;
	unsigned char *cbuf;
	struct shaper_rpmsg_tty_port *port = dev_get_drvdata(&rpdev->dev);

	/* flush the recv-ed none-zero data to tty node */
	if (len == 0)
		return 0;

	dev_dbg(&rpdev->dev, "msg(<- src 0x%x) len %d\n", src, len);

	print_hex_dump(KERN_DEBUG, __func__, DUMP_PREFIX_NONE, 16, 1, data, len, true);

	spin_lock_bh(&port->rx_lock);
	space = tty_prepare_flip_string(&port->port, &cbuf, len);
	if (space <= 0) {
		dev_err(&rpdev->dev, "No memory for tty_prepare_flip_string\n");
		spin_unlock_bh(&port->rx_lock);
		return -ENOMEM;
	}

	memcpy(cbuf, data, len);
	tty_flip_buffer_push(&port->port);
	spin_unlock_bh(&port->rx_lock);

	return 0;
}

static struct tty_port_operations shaper_rpmsg_tty_port_ops = {};

static int shaper_rpmsg_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct shaper_rpmsg_tty_port *cport = driver->driver_state;

	return tty_port_install(&cport->port, driver, tty);
}

static int shaper_rpmsg_tty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(tty->port, tty, filp);
}

static void shaper_rpmsg_tty_close(struct tty_struct *tty, struct file *filp)
{
	return tty_port_close(tty->port, tty, filp);
}

static int shaper_rpmsg_tty_write(struct tty_struct *tty, const unsigned char *buf, int total)
{
	int count;
	const unsigned char *tbuf;
	int ret = 0;
	struct shaper_rpmsg_tty_port *tty_port = container_of(tty->port, struct shaper_rpmsg_tty_port, port);
	struct rpmsg_device *rpdev = tty_port->rpdev;

	if (buf == NULL) {
		pr_err("buf shouldn't be null.\n");
		return -ENOMEM;
	}

	count = total;
	tbuf = buf;
	do {
		/* send a message to our remote processor */
		ret = rpmsg_send(rpdev->ept, (void*)tbuf, count > RPMSG_MAX_SIZE ? RPMSG_MAX_SIZE : count);
		if (ret) {
			dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
			return ret;
		}

		if (count > RPMSG_MAX_SIZE) {
			count -= RPMSG_MAX_SIZE;
			tbuf += RPMSG_MAX_SIZE;
		} else
			count = 0;

	} while (count > 0);

	return total;
}

static int shaper_rpmsg_tty_write_room(struct tty_struct *tty)
{
	/* report the space in the rpmsg buffer */
	return RPMSG_MAX_SIZE;
}

static const struct tty_operations shaper_rpmsg_tty_ops =
{
	.install = shaper_rpmsg_tty_install,
	.open = shaper_rpmsg_tty_open,
	.close = shaper_rpmsg_tty_close,
	.write = shaper_rpmsg_tty_write,
	.write_room = shaper_rpmsg_tty_write_room,
};

static int shaper_rpmsg_tty_probe(struct rpmsg_device *rpdev)
{
	int ret;
	struct shaper_rpmsg_tty_port *cport;
	struct tty_driver *tty_driver;

	dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x!\n", rpdev->src, rpdev->dst);

	cport = devm_kzalloc(&rpdev->dev, sizeof(*cport), GFP_KERNEL);
	if (!cport)
		return -ENOMEM;

	tty_driver = tty_alloc_driver(1, TTY_DRIVER_UNNUMBERED_NODE);
	if (IS_ERR(tty_driver)) {
		kfree(cport);
		return PTR_ERR(tty_driver);
	}

	tty_driver->driver_name = "rpmsg_tty";
	tty_driver->name = kasprintf(GFP_KERNEL, "ttyRPMSG%d", rpdev->dst);
	tty_driver->major = UNNAMED_MAJOR;
	tty_driver->minor_start = 0;
	tty_driver->type = TTY_DRIVER_TYPE_CONSOLE;
	tty_driver->init_termios = tty_std_termios;

	tty_set_operations(tty_driver, &shaper_rpmsg_tty_ops);

	tty_port_init(&cport->port);
	cport->port.ops = &shaper_rpmsg_tty_port_ops;
	spin_lock_init(&cport->rx_lock);
	cport->port.low_latency = cport->port.flags | ASYNC_LOW_LATENCY;
	cport->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, cport);
	tty_driver->driver_state = cport;
	cport->rpmsg_tty_drv = tty_driver;

	ret = tty_register_driver(cport->rpmsg_tty_drv);
	if (ret < 0) {
		pr_err("Couldn't install rpmsg tty driver: ret %d\n", ret);
		goto error1;
	}
	pr_info("installed rpmsg tty driver\n");

	/*
	 * send a message to our remote processor, and tell remote
	 * processor about this channel
	 */
	ret = rpmsg_send(rpdev->ept, MSG, strlen(MSG));
	if (ret) {
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
		goto error;
	}

	return 0;

error:
	tty_unregister_driver(cport->rpmsg_tty_drv);

error1:
	put_tty_driver(cport->rpmsg_tty_drv);
	tty_port_destroy(&cport->port);
	cport->rpmsg_tty_drv = NULL;
	kfree(cport);

	return ret;
}

static void shaper_rpmsg_tty_remove(struct rpmsg_device *rpdev)
{
	struct shaper_rpmsg_tty_port *cport = dev_get_drvdata(&rpdev->dev);

	dev_info(&rpdev->dev, "rpmsg tty driver is removed\n");

	tty_unregister_driver(cport->rpmsg_tty_drv);
	kfree(cport->rpmsg_tty_drv->name);
	put_tty_driver(cport->rpmsg_tty_drv);
	tty_port_destroy(&cport->port);
	cport->rpmsg_tty_drv = NULL;
}

static struct rpmsg_device_id shaper_rpmsg_driver_tty_id_table[] =
{
	{.name = "shaper-rpmsg-tty-channel-1",},
	{.name = "shaper-rpmsg-tty-channel",},
	{},
};

static struct rpmsg_driver rpmsg_tty_driver =
{
	.drv.name = KBUILD_MODNAME,
	.drv.owner = THIS_MODULE,
	.id_table = shaper_rpmsg_driver_tty_id_table,
	.probe = shaper_rpmsg_tty_probe,
	.callback = shaper_rpmsg_tty_cb,
	.remove = shaper_rpmsg_tty_remove,
};

static int __init shaper_rpmsg_tty_init(void)
{
	return register_rpmsg_driver(&rpmsg_tty_driver);
}

static void __exit shaper_rpmsg_tty_fini(void)
{
	unregister_rpmsg_driver(&rpmsg_tty_driver);
}
module_init( shaper_rpmsg_tty_init);
module_exit( shaper_rpmsg_tty_fini);

MODULE_AUTHOR("Shaper Tools, Inc.");
MODULE_DESCRIPTION("iMX virtio rpmsg tty driver");
MODULE_LICENSE("GPL v2");
