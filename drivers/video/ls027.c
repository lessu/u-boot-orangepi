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
#define GRAY8_TO_MONO_SIMPLE_CUT_OFF 64
/**
 * 
 * screen ops
 * 
*/
static uint8_t xrgb888_to_gray8_simple(uint8_t *xrgb888){
	/** fastest,ignore gamma etc */
	return xrgb888[1] /3 + xrgb888[2]/3 + xrgb888[3]/3;
}

static uint8_t gray8_to_mono_simple(uint8_t gray){
	return gray > GRAY8_TO_MONO_SIMPLE_CUT_OFF;
}

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

static int sharp_memory_spi_line(struct spi_slave *slave,uint8_t addr,void *line_bf)
{
	uint8_t dout[1 + 1 + LS027_WIDTH/8 + 2] = {
		//M1 M2 M3
		0b10000000,
		// address
		addr,
		// 50byte display

		// tail

	};
	memcpy(dout + 2,line_bf,LS027_WIDTH/8);
	dout[2 + LS027_WIDTH/8] = 0x00;
	dout[2 + LS027_WIDTH/8 +1] = 0x00;

	int ret = spi_xfer(slave, sizeof(dout) * 8, dout, 0, SPI_XFER_ONCE);
	return ret;
}
static inline uint8_t sharp_memory_reverse_byte(uint8_t b)
{
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}
static int s_framebuffer_convert(const uint32_t *fb_384000,void *output_12960){

	uint8_t *target = output_12960;
	for(int y = 0 ; y < LS027_HEIGHT; y++){
		*target = 0b10000000;
		target ++;
		*target = sharp_memory_reverse_byte(y+1);
		for(int x = 0 ; x < LS027_WIDTH; x++){
			uint32_t xrgb888 = fb_384000[x + y * LS027_WIDTH];
			uint8_t gray8 = xrgb888_to_gray8_simple( (uint8_t *)&xrgb888 );
			
			if((x % 8) == 0){
				target ++;
				*target = 0;
			}else{
				*target = (*target) << 1;
			}
			*target |= gray8_to_mono_simple(gray8) ? 1 : 0;
		}
		*target = 0;
		target ++;
		*target = 0;
	}
	return 0;
}

static bool s_is_buffer_all_zero(const void * buffer,uint32_t size){
	bool all_zero = true;
	const uint8_t *p = (const uint8_t *)buffer;
	while(size--){
		if(*p != 0){
			all_zero = false;
			break;
		}
		p++;
	}
	return all_zero;
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
		log_err("clear_screen failed %d",ret);
	}

	// spi_release_bus(dv_priv->spi); not get called 
	return 0;
}

static int lmp027_ops_sync(struct udevice *dev){
	
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct ls027_video_priv *dv_priv = dev_get_priv(dev);

	uint32_t *p = (uint32_t*)uc_plat->base;

	if(s_is_buffer_all_zero((const void *)uc_plat->base,uc_plat->size)){
		return 0;
	}

	static uint8_t mono_bf[LS027_HEIGHT * ( LS027_WIDTH  / 8 + 4 ) ];
	s_framebuffer_convert( (uint32_t*)uc_plat->base, mono_bf);

	spi_xfer(dv_priv->spi, sizeof(mono_bf) * 8, mono_bf, 0, SPI_XFER_ONCE);
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
