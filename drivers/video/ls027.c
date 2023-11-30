#include <common.h>
#include <env.h>
#include <dm.h>
#include <malloc.h>
#include <video.h>
#include <video_fb.h>
#include <log.h>
#include <stdint.h>
#include <asm/gpio.h>
#include <spi.h>
#include <time.h>
#include <linux/delay.h>
#define LS027_WIDTH 400
#define LS027_HEIGHT 240
/**
 * 
 * screen ops
 * 
*/
static int sharp_memory_spi_clear_screen(struct spi_slave *slave)
{
	uint8_t dout[2] = {
		//M1 M2 M3
		0b00100000,
		0b00000000
	};
	uint8_t din[2];
	int ret = spi_xfer(slave, sizeof(dout) * 8, dout, din, SPI_XFER_ONCE);
	return ret;
}

/**
 * 
 * U boot
 * DM
 * 
 * 
*/
struct ls027_video_priv {
	struct gpio_desc gpio_disp;
	struct gpio_desc gpio_vcom;
	struct spi_slave *spi; 
};

static int ls027_video_bind(struct udevice *dev){
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	uc_plat->size = LS027_WIDTH * LS027_HEIGHT * 4;
	return 0;
}

static int ls027_video_probe(struct udevice *dev){
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct video_priv    *uc_priv = dev_get_uclass_priv(dev);
	log_info("ls027_video_probe");

	/** uclass initialize */
	uc_priv->xsize = LS027_WIDTH;
	uc_priv->ysize = LS027_HEIGHT;
	uc_priv->bpix  = VIDEO_BPP32;
	memset((void*)uc_plat->base,0,uc_plat->size);	
	
	/** driver initialize */
	struct ls027_video_priv *dv_priv = dev_get_priv(dev);
	int ret = 0;

	ret = gpio_request_by_name(dev, "disp-gpios", 0, &dv_priv->gpio_disp, GPIOD_IS_OUT_ACTIVE);
	if (ret) return ret;
	
	ret = gpio_request_by_name(dev, "vcom-gpios", 0, &dv_priv->gpio_vcom, GPIOD_IS_OUT);
	if (ret) return ret;


	/** spi */
	
	struct spi_slave *slave = dev_get_parent_priv(dev);
	dv_priv->spi = slave;
	/* Claim spi bus */
	ret = spi_claim_bus(slave);
	if (ret) return ret;

	// Power up sequence
	dm_gpio_set_value(&dv_priv->gpio_disp, 1);
	dm_gpio_set_value(&dv_priv->gpio_vcom, 0);
	udelay(5000);

	ret = sharp_memory_spi_clear_screen(dv_priv->spi);
	if( ret ){
		log_info("clear_screen failed %d",ret);
	}

	// spi_release_bus(dv_priv->spi); not get called 
	return 0;
}

static int lmp027_ops_sync(struct udevice *dev){
	log_info("ls027_sync");
	
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct ls027_video_priv *dv_priv = dev_get_priv(dev);

	uint32_t *p = (uint32_t*)uc_plat->base;
	bool all_zero = true;
	for(int y = 0; y < LS027_HEIGHT;y ++){
		for(int x = 0;x < LS027_WIDTH;x++){
			if( *p != 0){
				all_zero = false;
				break;			
			}
			p++;
		}
	}
	log_info(",all_zero=%d\n",all_zero);
	return 0;
}

static const struct udevice_id ls027_video_ids[] = {
	{ .compatible = "sharp,ls027" },
	{ /* esentinel */ }
};

struct video_ops lmp027_ops={
	.video_sync = lmp027_ops_sync
};


U_BOOT_DRIVER(ls027_video) = {
	.name   = "sharp_ls027",
	.id     = UCLASS_VIDEO,
	.of_match = ls027_video_ids,
	.bind	= ls027_video_bind,
	.priv_auto	= sizeof(struct ls027_video_priv),
	.probe  = ls027_video_probe,
	.flags  = DM_FLAG_PRE_RELOC | DM_FLAG_OS_PREPARE,
	.ops    = &lmp027_ops
};
