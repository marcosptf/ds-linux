/*
 *  linux/drivers/mmc/dsmemsd.c - DSMEM SD driver
 *
 *  Copyright (C) 2006 Amadeus, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/mmc/host.h>
#include <linux/mmc/protocol.h>

#include <asm/dma.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/scatterlist.h>
#include <asm/sizes.h>

#ifdef CONFIG_MMC_DEBUG
#define DBG(x...)	printk(x)
#else
#define DBG(x...)	do { } while (0)
#endif

#define DRIVER_NAME	"dsmemsd"
#define DRIVER_VERSION	"1.0.2"

/*****************************************************************************/
/* IO registers */
#define WAIT_CR   (*(volatile u16 *) 0x04000204)

#define SDCONTROL (*(volatile  u8 *) 0x0A00C000) /* SD card control port */
#define SDC_POWER 0x01		/* bit 0: 1 = power active */
#define SDC_CS	  0x02		/* bit 1: 1 = SD card selected */
#define SDC_ACTIV 0x04		/* bit 2: 1 = SD card outputs enabled */

#define SDSTATUS  (*(volatile  u8 *) 0x0A00C000) /* SD card status  port */
#define SDS_BUSY  0x01		/* bit 0: 1 = SPI busy, transfer in progress */

#define SDDATA    (*(volatile  u8 *) 0x0A00E000) /* SD card data port */

/*****************************************************************************/
/* translation of the SD mode commands to SPI mode commands */

/* opcode bits */
#define OPCODE_MASK 	0x3F

/* SPI response codes == number of response bytes */
#define R1  0x1000	
#define R1B 0x1100
#define R2  0x2000
#define R3  0x5000
#define RESPONSE(x) ((x) >> 12)
#define REPSONSE_BUSY	0x100

static const u16 spi_ops[64] ={
/******************************** Basic command set **************************/
/* Reset cards to idle state CMD0 */
(0  | R1),
/* Send_OP_COND (MMC mode, do not use for SD cards) CMD1 */
(1  | R1),
/* Card sends CID CMD2 */
/* Mapped to CM10, not available in SPI mode */
(10 | R1),
/* 3 .. 8: invalid */
0,0,0,0,0,0,
/* Card sends the CSD CMD9 */
(9  | R1),
/* Card sends CID CMD10 */
(10 | R1),
/* 11: invalid */
0,
/* Stop a multiple block (stream) read/write operation CMD12 */
(12 | R1B),
/* Get the addressed card’s status register (A)CMD13 */
(13 | R2),
/* 14-15: invalid */
0,0,
/***************************** Block read commands **************************/
/* Set the block length CMD16 */
(16 | R1),
/* Read a single block CMD17 */
(17 | R1),
/* Read multiple blocks until a CMD12: CMD18 */
(18 | R1),
/* 19-21: invalid */
0,0,0,
/* Get the number of written write blocks (Minus errors ) ACMD22 */
(22 | R1),
/* Set the number of write blocks to be pre-erased before writing ACMD23 */
(23 | R1),
/***************************** Block write commands *************************/
/* Write a block of the size selected with CMD16: CMD24 */
(24 | R1),
/* Multiple block write until a CMD12: CMD25 */
(25 | R1),
/* 26: invalid */
0,
/* Program the programmable bits of the CSD: CMD27 */
(27 | R1),
/***************************** Write protection *****************************/
/* Set the write protection bit of the addressed group CMD28 */
(28 | R1B),
/* Clear the write protection bit of the addressed group CMD29 */
(29 | R1B),
/* Ask the card for the status of the write protection bits CMD30 */
(30 | R1),
/* 31: invalid */
0,
/***************************** Erase commands *******************************/
/* Set the address of the first write block to be erased CMD32 */
(32 | R1),
/* Set the address of the last write block to be erased CMD33 */
(33 | R1),
/* 34-37: invalid */
0,0,0,0,
/* Erase the selected write blocks CMD38 */
(38 | R1B),
/* 39-40: invalid */
0,0,
/* Get the card’s OCR (SD mode) ACMD41 */
(41 | R1),
/* Connect or disconnect the 50kOhm internal pull-up on CD/DAT[3] ACMD42 */
(42 | R1),
/***************************** Lock Card commands ***************************/
/* Commands from 43 to 50, not defined here */
0,0,0,0,0,0,0,0,
/* Get the SD configuration register ACMD51 */
(51 | R1),
/* Commands from 52 to 54, not defined here */
0,0,0,
/***************************** Application-specific commands ****************/
/* Flag that the next command is application-specific CMD55 */
(55 | R1),
/* General purpose I/O for application-specific commands CMD56 */
(56 | R1),
/* 57: invalid */
0,
/* Read the OCR (SPI mode only) CMD58 */
(58 | R3),
/* Turn CRC on or off CMD59 */
(59 | R1),
/* 60-63: invalid */
0,0,0,0
};

/*****************************************************************************/
/* SPI primitives */

/* wait until the SPI is idle */
#define spi_wait() \
	while (SDSTATUS & SDS_BUSY) {}

/* send a byte to SPI */
#define spi_send(b) \
	SDDATA = (b)

/* receive a byte from SPI */
#define spi_rec() \
	SDDATA

// send a byte to SD, wait until done
#define sd_send(b) \
	{spi_send(b); spi_wait();}

/*****************************************************************************/
/* Control primitives */

/* set SD to powerdown */
#define sd_off() \
	SDCONTROL = 0x00

/* set SD power on */
#define sd_pon() \
	SDCONTROL = SDC_POWER

/* set SD active, but not selected */
#define sd_csoff() \
	SDCONTROL = (SDC_POWER | SDC_ACTIV)

/* select SD card */
#define sd_cson() \
	SDCONTROL = (SDC_POWER | SDC_ACTIV | SDC_CS)

/*****************************************************************************/

/* transfer a byte to/from SD */
static inline u8 sd_transfer(u8 data)
{
	// send out the data and wait
	sd_send(data);
	// return the result
	return spi_rec();
}

/* send FF for a specified amount of bytes */
static inline void sd_clocks(int clocks) 
{
	for ( ; clocks; clocks--)
		sd_send(0xFF);
}

/* receive a byte from SD, sending FF */
static inline unsigned char sd_receive(void)
{
	return sd_transfer(0xFF);
}

/*****************************************************************************/

struct dsmemsd_host {
	struct mmc_host		*mmc;
	u8			power_mode;
	u8			idle;
	u8			cids;
};

/* send a SD command in SPI mode */
/* @return 0 for success, != 0 otherwise */
static int dsmemsd_send_cmd(struct mmc_command *cmd)
{
	int i;
	u8  response[5];
     	u8  tmp;
	u16 opcode; 
   	
	/* clear the response flags */
	memset(cmd->resp, 0, sizeof(cmd->resp));

	/* Read and translate the opcode to SPI.
	   This gives us the SPI response code, 
	   response byte count and busy flag. */
	opcode = spi_ops[cmd->opcode];

	/* Check if we can support this command */
	if (!opcode) {
		/* no: fake an answer */
		cmd->resp[0] = (0x200 | R1_APP_CMD | R1_READY_FOR_DATA);
		cmd->error = MMC_ERR_NONE;
		return 0;
	}
		
	/* activate chip select. We are in SPI mode */
	sd_cson();

	DBG("Sending Opcode %d Arg %d\n", opcode & OPCODE_MASK, cmd->arg);
	/* send the command */
	sd_send((u8)((opcode & OPCODE_MASK) | 0x40));
	sd_send((u8)(cmd->arg >> 24));
	sd_send((u8)(cmd->arg >> 16));
	sd_send((u8)(cmd->arg >> 8));
	sd_send((u8)(cmd->arg));

	/* This is the CRC. It only matters what we put here for the first
	command. Otherwise, the CRC is ignored for SPI mode unless we
	enable CRC checking. */
	sd_send(0x95);

	/* Wait for a response. A response can be recognized by the
	start bit (a zero) */
	/* NCR = 8 Bytes, We use some more... */
	for (i = 16; ; i--) {
		if (!i) {
			/* Timeout */
			sd_csoff();
			DBG("Timeout waiting for a response\n");
			cmd->error = MMC_ERR_TIMEOUT;
			return -1;
		}
		tmp = sd_receive();
		if (!(tmp & 0x80))
			break;
	}

	/* Read the response. */
	memset(response, 0, sizeof(response));
  	for (i=0; i < RESPONSE(opcode); i++) {
		response[i] = tmp;
		/* This handles the trailing-byte requirement. */
		tmp = sd_receive();
	}
	DBG("Got native response %X %X %X %X %X\n", (int)response[0],(int)response[1],(int)response[2],(int)response[3],(int)response[4]);

	/* translate the response from SPI mode to SD mode */
	switch (cmd->flags) {
	case MMC_RSP_R1:
	case MMC_RSP_R1B:
		/* standard error codes */
		if (!(response[0] & 0x01)) cmd->resp[0] |= 0x200;  /* translate idle to !ready */
		if (response[0] & 0x02) cmd->resp[0] |= R1_ERASE_RESET; 
		if (response[0] & 0x04) cmd->resp[0] |= R1_ILLEGAL_COMMAND; 
		if (response[0] & 0x08) cmd->resp[0] |= R1_COM_CRC_ERROR; 
		if (response[0] & 0x10) cmd->resp[0] |= R1_ERASE_SEQ_ERROR; 
		if (response[0] & 0x20) cmd->resp[0] |= R1_ADDRESS_ERROR;
		if (response[0] & 0x40) cmd->resp[0] |= R1_BLOCK_LEN_ERROR;
		if (response[1] & 0x01) cmd->resp[0] |= R1_CARD_IS_LOCKED; 
		if (response[1] & 0x02) cmd->resp[0] |= R1_LOCK_UNLOCK_FAILED; 
		if (response[1] & 0x04) cmd->resp[0] |= R1_ERROR; 
		if (response[1] & 0x08) cmd->resp[0] |= R1_CC_ERROR; 
		if (response[1] & 0x10) cmd->resp[0] |= R1_CARD_ECC_FAILED; 
		if (response[1] & 0x20) cmd->resp[0] |= R1_WP_VIOLATION;
		if (response[1] & 0x40) cmd->resp[0] |= R1_ERASE_PARAM; 
		if (response[1] & 0x80) cmd->resp[0] |= R1_OUT_OF_RANGE; 
		/* Set this bit always. Else there are no ACMDs send to the SD. */
		cmd->resp[0] |= R1_APP_CMD;
		/* Set this bit always. Else the hosts loops forever after each data transfer */
		cmd->resp[0] |= R1_READY_FOR_DATA;
		break;
	case MMC_RSP_R2:
		/* CID/CSD codes */
		/* waiting for the data token */
		/* NCR = 8 Bytes, We use some more... */
		for (i = 16; ; i--) {
			if (!i) {
				/* Timeout */
				sd_csoff();
				DBG("Timeout waiting for data token\n");
				cmd->error = MMC_ERR_TIMEOUT;
				return -1;
			}
			tmp = sd_receive();
			if (tmp == 0xFE)
				break;
		}
		
		/* skip the data token */
		tmp = sd_receive();
		/* 16 bytes are waiting for us from the SD card.*/
		for (i=0; i < 4; i++)
		{
			cmd->resp[i]  = (tmp << 24); tmp = sd_receive();
			cmd->resp[i] |= (tmp << 16); tmp = sd_receive();
			cmd->resp[i] |= (tmp << 8);  tmp = sd_receive();
			cmd->resp[i] |=  tmp;        tmp = sd_receive();
		}
		/* skip CRC */
		tmp = sd_receive();
		tmp = sd_receive();
		break;
	case MMC_RSP_R3:
		/* OCR register */
		cmd->resp[0]  = response[4];
		cmd->resp[0] |= response[3] << 8;
		cmd->resp[0] |= response[2] << 16;
		cmd->resp[0] |= response[1] << 24;
		break;
	default:/* nothing */
		break;
	}
	
	/* If the response is a "busy" type, then there’s some
	* special handling that needs to be done. The card will
	* output a continuous stream of zeros, so the end of the BUSY
	* state is signaled by any nonzero response. The bus idles
	* high.
	*/
	if (opcode & REPSONSE_BUSY) {
		/* This gives 250ms timeout */
		for (i = 500000; ;i--) {
			if (!i) {
				/* Timeout */
				sd_csoff();
				DBG("Timeout waiting for NON_BUSY\n");
				cmd->error = MMC_ERR_TIMEOUT;
				return -1;
			}
			tmp = sd_receive();
			if (tmp == 0xFF)
				break;
		}
	}

	/* deselect the card */
	sd_csoff();

	// success
	cmd->error = MMC_ERR_NONE;
	return 0;
}

/*
 * Transfer data blocks from/to sd card.
 */
static void dsmemsd_xfer( struct mmc_data *data )
{
	int sgindex;
	int size = data->blocks << data->blksz_bits;
	struct scatterlist *sg = data->sg;
	int i;
	u8 multiwrite = size > 512;
	u8 tmp = 0;

     	DBG("%s Bytes=%d\n",__FUNCTION__, size);

	/* activate chip select. We are in SPI mode */
	sd_cson();

	/* Iterate through the s/g list */
	data->bytes_xfered = 0;
	data->error = MMC_ERR_NONE;
	for (sgindex = 0; sgindex < data->sg_len; sgindex++) {
		u8 *p;
		int sglength;

		/* get the start address */
		p = page_address(sg[sgindex].page)+sg[sgindex].offset;

		/* compute the length */
		sglength = sg[sgindex].length;
		if (size < sglength)
			sglength = size;

		/* transfer the physical sectors */
		while (sglength > 0) {
			int length = sglength;
			/* break down to sector length */
			if (length > 512)
				length = 512;
			
			/* do the transfer */
			if (data->flags & MMC_DATA_READ) {
		
				/* Wait for the data token */
				/* This gives 100ms timeout */
				for (i = 200000; ;i--) {
					if (!i) {
						/* Timeout */
						sd_csoff();
						DBG("Timeout waiting for read data token\n");
						data->error = MMC_ERR_TIMEOUT;
						return;
					}
					tmp = sd_receive();
					if (tmp != 0xFF)
						break;
				}
				/* Error? */
				if (tmp != 0xFE) {
					sd_csoff();
					DBG("Error data token read %d\n", (int)tmp);
					data->error = MMC_ERR_FAILED;
					return;
				}
		
				/* read the data */
				for (i = 0; i < length; i++) {
					*p++ = sd_receive();
				}
				/* skip the CRC */
				sd_receive();
				sd_receive();
			} else if (data->flags & MMC_DATA_WRITE) {
		
				/* Send the data token */
				if (multiwrite) 
					sd_send(0xFC)
				else
					sd_send(0xFE);
		
				/* Send the data */
				for (i = 0; i < length; i++) {
					sd_send(*p++);
				}
		
				/* skip the CRC */
				sd_send(0x00);
				sd_send(0x00);

				/* Wait for the data response token */
				/* NCR = 8 Bytes, We use some more... */
				for (i = 16; ; i--) {
					if (!i) {
						/* Timeout */
						sd_csoff();
						DBG("Timeout waiting for a data response\n");
						data->error = MMC_ERR_TIMEOUT;
						return;
					}
					tmp = sd_receive();
					if ((tmp & 0x11) == 0x01)
						break;
				}
				if ((tmp & 0x0E) != 0x04) {
					sd_csoff();
					DBG("Error data response %d\n", (int)tmp);
					data->error = MMC_ERR_FAILED;
					return;
				}

				/* Wait until not busy */
				/* This gives 250ms timeout */
				for (i = 500000; ;i--) {
					if (!i) {
						/* Timeout */
						sd_csoff();
						DBG("Timeout waiting for NON_BUSY\n");
						data->error = MMC_ERR_TIMEOUT;
						return;
					}
					tmp = sd_receive();
					if (tmp == 0xFF)
						break;
				}
			}
	
			/* ready with one physical sector */
			data->bytes_xfered += length;
			size -= length;
			sglength -= length;
		}

		/* stop if all done */
		if (size <= 0) break;
	}

	/* for multiple writes, append the stop token */
	if ((data->flags & MMC_DATA_WRITE) && multiwrite) {
		sd_send(0xFD);
		/* Wait until not busy */
		/* This gives 250ms timeout */
		for (i = 500000; ;i--) {
			if (!i) {
				/* Timeout */
				sd_csoff();
				DBG("Timeout waiting for NON_BUSY\n");
				data->error = MMC_ERR_TIMEOUT;
				return;
			}
			tmp = sd_receive();
			if (tmp == 0xFF)
				break;
		}
	}

	/* Add additional 8 clocks */
	sd_clocks(1);

	/* deselect the card */
	sd_csoff();

	DBG("xfer done\n");
}

static void dsmemsd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct dsmemsd_host *host = mmc_priv(mmc);
	int ret;
	int i;

    	DBG("%s Opcode %d\n",__FUNCTION__, mrq->cmd->opcode);

	/* what command should we send? */
	switch (mrq->cmd->opcode) {
	case MMC_GO_IDLE_STATE:
		host->idle = 1;
		host->cids = 0;
		break;
	
	case MMC_APP_CMD:
		if (host->idle == 1) {
			host->idle = 0;

			/* in SPI mode, we need to send ACMD 41 (looped) */
			/* This gives a timeout of 1000ms */	
			for (i = 0; i < 100; i++) {
				struct mmc_command cmd2;
				cmd2.opcode = MMC_APP_CMD;
				cmd2.arg    = 0;
				cmd2.flags  = MMC_RSP_R1;
				cmd2.retries= 0;
				cmd2.data   = NULL;
				ret = dsmemsd_send_cmd(&cmd2);
				if (ret)
					continue;
				cmd2.opcode = SD_APP_OP_COND;
				cmd2.arg    = 0;
				cmd2.flags  = MMC_RSP_R1;
				cmd2.retries= 0;
				cmd2.data   = NULL;
				ret = dsmemsd_send_cmd(&cmd2);
				if (ret)
					continue;
				if (R1_CURRENT_STATE(cmd2.resp[0]) != 0) {
					/* We know that next command is SD_APP_OP_COND.
					   This will be translated from a ACMD to a CMD.
					   So leave here in CMD state. */
					memset(mrq->cmd->resp, 0, sizeof(mrq->cmd->resp));
 					mrq->cmd->resp[0] = R1_APP_CMD | R1_READY_FOR_DATA;
					mrq->cmd->error = MMC_ERR_NONE;
					mmc_request_done(mmc, mrq);
					return;
 				}
				msleep_interruptible(10);
			}					
		}		
		break;
	case SD_APP_OP_COND:
		/* in SPI mode, we need another opcode for reading the OP_COND */
		mrq->cmd->opcode = 58;
		break;
	case MMC_ALL_SEND_CID:
		/* we need to create a timeout for the second card */
		if (host->cids != 0) {
			memset(mrq->cmd->resp, 0, sizeof(mrq->cmd->resp));
			mrq->cmd->retries = 0;
			mrq->cmd->error = MMC_ERR_TIMEOUT;
			mmc_request_done(mmc, mrq);
			return;	
		} else {
			host->cids++;
		}
		break;
	default: /* nothing */
		break;
	}

    	/* Send the command to the card */
	ret = dsmemsd_send_cmd(mrq->cmd);
	if (!ret) {
		if ( mrq->data ) {
			dsmemsd_xfer( mrq->data );
			if (mrq->stop) {
				dsmemsd_send_cmd(mrq->stop);
			}
		}
    	}
    	mmc_request_done(mmc, mrq);
}

/* read the write protect switch */
static int dsmemsd_get_ro(struct mmc_host *mmc)
{
	DBG("%s\n",__FUNCTION__);
	/* can't distinguish between ro and rw */
	return 0;
}

/* set operating conditions for the host */
static void dsmemsd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct dsmemsd_host *host = mmc_priv(mmc);
    	DBG("%s\n",__FUNCTION__);

    	DBG("bus_mode    = %x\n", ios->bus_mode);
    	DBG("chip_select = %x\n", ios->chip_select);
    	DBG("bus_width   = %x\n", ios->bus_width);

	DBG("dsmemsd_set_ios: clock %u power %u vdd %u.%01u\n",
	    ios->clock, ios->power_mode, (ios->vdd-4+17) / 10,
	    (ios->vdd-4+17) % 10);

	/* detect power mode change */
	if (host->power_mode != ios->power_mode) {
		switch (ios->power_mode) {
		case MMC_POWER_OFF:
			sd_off();
			break;	
		case MMC_POWER_UP:
			/* set PHI = 16 MHz */
			/* This is also needed for RS232, so we can't switch it off again */
			WAIT_CR |= 0x0060;
		
			/* apply power */
			sd_pon();

			/* minimum wait 1ms to come up */
			/* This is to avoid having /CS powering the SD card during power up */
			/* this delay is in mmc_power_up() */
			break;
		case MMC_POWER_ON:
			/* drive /CS output to inactive = HIGH */
			sd_csoff();
			/* send some clock pulses to SD */
			sd_clocks(100);
			break;
		}
		host->power_mode = ios->power_mode;
	}

	/* ignore chip select changes - we are SPI and need
	   a different chip select handling than in SD mode */

	/* ignore clock - only one speed available */
	/* ignore vdd - only 3.3 volts available */
	/* ignore bus width - only 1 bit available */
	/* ignore bus mode - only push-pull available */
}

static struct mmc_host_ops dsmemsd_ops = {
	.request	= dsmemsd_request,
	.get_ro		= dsmemsd_get_ro,
	.set_ios	= dsmemsd_set_ios,
};

/* setup the SD host controller */
static int dsmemsd_probe(struct device *dev)
{
	struct mmc_host *mmc;
	struct dsmemsd_host *host = NULL;
    	DBG("%s\n",__FUNCTION__);

        /* allocate host data structures */
	mmc = mmc_alloc_host(sizeof(struct dsmemsd_host), dev);
	if (!mmc) {
		return -ENOMEM;
	}

	/* populate host data structures */
	mmc->ops = &dsmemsd_ops;

	/* GBA slot PHI max. Baudrate */
	mmc->f_min = 16777216;
	mmc->f_max = 16777216;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = 0;
	mmc->mode = MMC_MODE_SD;

	/* As we are doing transfers in software, we have no real limits.
	   So use some big numbers here. */
	mmc->max_seg_size = 0x10000;
	mmc->max_hw_segs  = 0x100;	
	mmc->max_phys_segs= 0x100;	
	mmc->max_sectors  = 0x100;	

	host = mmc_priv(mmc);
	host->mmc = mmc;

	dev_set_drvdata(dev, mmc);
	mmc_add_host(mmc);

	return 0;
}

static int dsmemsd_remove(struct device *dev)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	dev_set_drvdata(dev, NULL);

	if (mmc) {
		mmc_remove_host(mmc);
		mmc_free_host(mmc);
	}
	return 0;
}

/* no power managment here */
#define dsmemsd_suspend	NULL
#define dsmemsd_resume	NULL

static void dsmemsd_platform_release(struct device * device){
	/* never */
}

static struct platform_device dsmemsd_device = {
    .name = DRIVER_NAME,
    .dev = {
        .release = dsmemsd_platform_release,
        }
};

static struct device_driver dsmemsd_driver = {
	.name		= DRIVER_NAME,
	.bus		= &platform_bus_type,
	.probe		= dsmemsd_probe,
	.remove		= dsmemsd_remove,
	.suspend	= dsmemsd_suspend,
	.resume		= dsmemsd_resume,
};

static int __init dsmemsd_init(void)
{
    int ret;
	ret = driver_register(&dsmemsd_driver);
    if (!ret) {
        ret = platform_device_register( &dsmemsd_device );
    }
    return ret ;
}

static void __exit dsmemsd_exit(void)
{
	driver_unregister(&dsmemsd_driver);
}

module_init(dsmemsd_init);
module_exit(dsmemsd_exit);

MODULE_DESCRIPTION("DSMEM SD Driver");
MODULE_LICENSE("GPL");
