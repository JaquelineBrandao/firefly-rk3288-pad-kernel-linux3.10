#include <media/camsys_head.h>

#include "camsys_cif.h"
#include "camsys_marvin.h"
#include "camsys_mipicsi_phy.h"
#include "camsys_gpio.h"
#include "camsys_soc_priv.h"

unsigned int camsys_debug=1;
module_param(camsys_debug, int, S_IRUGO|S_IWUSR);

static int drv_version = CAMSYS_DRIVER_VERSION;
module_param(drv_version, int, S_IRUGO);
static int head_version = CAMSYS_HEAD_VERSION;
module_param(head_version, int, S_IRUGO);


typedef struct camsys_devs_s {
    spinlock_t lock;
    struct list_head devs;
} camsys_devs_t;

static camsys_devs_t camsys_devs;

static int camsys_i2c_write(camsys_i2c_info_t *i2cinfo, camsys_dev_t *camsys_dev)
{
    int err = 0,i,j;
    unsigned char buf[8],*bufp;
    unsigned short msg_times,totallen,onelen;
    struct i2c_msg msg[1];
    struct i2c_adapter *adapter;
    
    adapter = i2c_get_adapter(i2cinfo->bus_num);
    if (adapter == NULL) {
        camsys_err("Get %d i2c adapter is failed!",i2cinfo->bus_num);
        err = -EINVAL;
        goto end;
    }
    
    if (i2cinfo->i2cbuf_directly) {
        if (camsys_dev->devmems.i2cmem == NULL) {
            camsys_err("%s has not i2c mem, it isn't support i2c buf write!",dev_name(camsys_dev->miscdev.this_device));
            err = -EINVAL;
            goto end;
        }
        totallen = (i2cinfo->i2cbuf_bytes&0xffff);
        onelen = (i2cinfo->i2cbuf_bytes&0xffff0000)>>16;
        msg_times = totallen/onelen;
        if (totallen > camsys_dev->devmems.i2cmem->size) {
            camsys_err("Want to write 0x%x bytes, i2c memory(size: 0x%x) is overlap",totallen,camsys_dev->devmems.i2cmem->size);
            err = -EINVAL;
            goto end;
        }
        bufp = (unsigned char*)camsys_dev->devmems.i2cmem->vir_base;        
    } else {
        for (i=0; i<i2cinfo->reg_size; i++) {
            buf[i] = (i2cinfo->reg_addr>>((i2cinfo->reg_size-1-i)*8))&0xff;
        }
        for (j=0; j<i2cinfo->val_size; j++) {
            buf[i+j] = (i2cinfo->val>>((i2cinfo->val_size-1-j)*8))&0xff;  /* ddl@rock-chips.com: v0.a.0 */
        }
        bufp = buf;
        onelen = i2cinfo->val_size + i2cinfo->reg_size;
        msg_times = 1;
    }
    
	err = -EAGAIN;    
    msg->addr = (i2cinfo->slave_addr>>1);
    msg->flags = 0;
    msg->scl_rate = i2cinfo->speed;
   // msg->read_type = 0; 
    msg->len = onelen;
    for (i=0; i<msg_times; i++) {        
        msg->buf = bufp+i*onelen;        
		err = i2c_transfer(adapter, msg, 1);    	
		if (err < 0) {
            camsys_err("i2c write dev(addr:0x%x) failed!",i2cinfo->slave_addr);
			udelay(10);
		}
    }

end:
    return err;
}

static int camsys_i2c_read(camsys_i2c_info_t *i2cinfo, camsys_dev_t *camsys_dev)
{
    int err = 0,i,retry=2,tmp, num_msg;
    unsigned char buf[8];
    struct i2c_msg msg[2];
    struct i2c_adapter *adapter;
    
    adapter = i2c_get_adapter(i2cinfo->bus_num);
    if (adapter == NULL) {
        camsys_err("Get %d i2c adapter is failed!",i2cinfo->bus_num);
        err = -EINVAL;
        goto end;
    } 

	num_msg = 0;
	if (i2cinfo->reg_size) {                /* ddl@rock-chips.com: v0.a.0 */
	    for (i=0; i<i2cinfo->reg_size; i++) {
	        buf[i] = (i2cinfo->reg_addr>>((i2cinfo->reg_size-1-i)*8))&0xff;
	    }
		
	    msg[0].addr = (i2cinfo->slave_addr>>1);
		msg[0].flags = 0;
		msg[0].scl_rate = i2cinfo->speed;
		//msg[0].read_type = 0;
	    msg[0].buf = buf;
	    msg[0].len = i2cinfo->reg_size;
		num_msg++;
	}
    
    msg[1].addr = (i2cinfo->slave_addr>>1);
	msg[1].flags = I2C_M_RD;
	msg[1].scl_rate = i2cinfo->speed;
//	msg[1].read_type = 0;
    msg[1].buf = buf;
    msg[1].len = (unsigned short)i2cinfo->val_size;
	err = -EAGAIN;    
	num_msg++;

	while ((retry-- > 0) && (err < 0)) {						 /* ddl@rock-chips.com :  Transfer again if transent is failed	 */
		if (num_msg==1) {			
			err = i2c_transfer(adapter, &msg[1], num_msg);
		} else {
			err = i2c_transfer(adapter, msg, num_msg);
		}
	
		if (err >= 0) {
            err = 0;
		} else {
			camsys_err("i2c read dev(addr:0x%x) failed,try again-%d!",i2cinfo->slave_addr,retry);
			udelay(10);
		}
	}


    if (err==0) { 
        i2cinfo->val = 0x00;
        for(i=0; i<i2cinfo->val_size; i++) {
            tmp = buf[i];
            i2cinfo->val |= (tmp<<((i2cinfo->val_size-1-i)*8));
        }
    }
    
end:
    return err;
}


static int camsys_extdev_register(camsys_devio_name_t *devio, camsys_dev_t *camsys_dev)
{
    int err = 0,i;
    camsys_extdev_t *extdev;
    camsys_regulator_info_t *regulator_info;
    camsys_regulator_t *regulator;
    camsys_gpio_info_t *gpio_info;
    camsys_gpio_t *gpio;
    
    if ((devio->dev_id & CAMSYS_DEVID_EXTERNAL) == 0) {
        err = -EINVAL;
        camsys_err("dev_id: 0x%x is not support for camsys!",devio->dev_id);
        goto end;
    }  

    extdev = camsys_find_extdev(devio->dev_id, camsys_dev);
    if (extdev != NULL) {
        if (strcmp(extdev->dev_name, devio->dev_name) == 0) {
            err = 0;
        } else {
            err = -EINVAL;    /* ddl@rock-chips.com: v0.0x13.0 */
            camsys_warn("Extdev(dev_id: 0x%x dev_name: %s) has been registered in %s!",
                extdev->dev_id, extdev->dev_name,dev_name(camsys_dev->miscdev.this_device));
        }
        goto end;
    }

    extdev = kzalloc(sizeof(camsys_extdev_t),GFP_KERNEL);
    if (extdev == NULL) {
        camsys_err("alloc camsys_extdev_t failed!");
        err = -ENOMEM;
        goto end;
    }
    
    extdev->dev_cfg = devio->dev_cfg;
    extdev->fl.fl.active = devio->fl.fl.active;
    regulator_info = &devio->avdd;
    regulator = &extdev->avdd;
    for (i=(CamSys_Vdd_Start_Tag+1); i<CamSys_Vdd_End_Tag; i++) {
        if (strcmp(regulator_info->name,"NC")) {
            regulator->ldo = regulator_get(NULL,regulator_info->name);
            if (IS_ERR_OR_NULL(regulator->ldo)) {
                camsys_err("Get %s regulator for dev_id 0x%x failed!",regulator_info->name,devio->dev_id);
                err = -EINVAL;
                goto fail;
            }
            
            regulator->min_uv = regulator_info->min_uv;
            regulator->max_uv = regulator_info->max_uv;
            camsys_trace(1,"Get %s regulator(min: %duv  max: %duv) for dev_id 0x%x success",
                        regulator_info->name,regulator->min_uv,regulator->max_uv,
                        devio->dev_id);
        } else {
            regulator->ldo = NULL;
            regulator->min_uv = 0;
            regulator->max_uv = 0;
        }

        regulator++;
        regulator_info++;
    }

    gpio_info = &devio->pwrdn;
    gpio = &extdev->pwrdn;
    for (i=(CamSys_Gpio_Start_Tag+1); i<CamSys_Gpio_End_Tag; i++) {
        if (strcmp(gpio_info->name,"NC")) {
            gpio->io = camsys_gpio_get(gpio_info->name);
            if (gpio->io < 0) {
                camsys_err("Get %s gpio for dev_id 0x%x failed!",gpio_info->name,devio->dev_id);
                err = -EINVAL;
                goto fail;
            }
            if (gpio_request(gpio->io,"camsys_gpio")<0) {
                camsys_err("Request %s(%d) failed",gpio_info->name,gpio->io);
            }
            gpio->active = gpio_info->active;
            camsys_trace(1,"Get %s(%d) gpio(active: %d) for dev_id 0x%x success!",
                        gpio_info->name,gpio->io,gpio->active,devio->dev_id);
        } else {
            gpio->io = 0xffffffff;
            gpio->active = 0xffffffff;
        }

        gpio++;
        gpio_info++;
    }

    extdev->pdev = camsys_dev->pdev;
    extdev->phy = devio->phy;
    extdev->clk = devio->clk;
    extdev->dev_id = devio->dev_id;
    //spin_lock(&camsys_dev->lock);
    mutex_lock(&camsys_dev->extdevs.mut);
    list_add_tail(&extdev->list, &camsys_dev->extdevs.list);
    //spin_unlock(&camsys_dev->lock);
    mutex_unlock(&camsys_dev->extdevs.mut);

    camsys_dev->iomux(extdev, (void*)camsys_dev);

    memcpy(extdev->dev_name,devio->dev_name, sizeof(extdev->dev_name));
    camsys_trace(1,"Extdev(dev_id: 0x%x  dev_name: %s) register success",
        extdev->dev_id,
        extdev->dev_name);

    return 0;
fail:
    if (extdev) { 
        kfree(extdev);
        extdev = NULL;
    }
end:
    
    return err;
}

static int camsys_extdev_deregister(unsigned int dev_id, camsys_dev_t *camsys_dev, bool all)
{
    int err = 0,i;
    camsys_extdev_t *extdev;
    camsys_regulator_t *regulator;
    camsys_gpio_t *gpio;

    if (all == false) {
        if ((dev_id & CAMSYS_DEVID_EXTERNAL) == 0) {
            err = -EINVAL;
            camsys_err("dev_id: 0x%x is not support for %s!",dev_id, dev_name(camsys_dev->miscdev.this_device));
            goto end;
        }

        extdev = camsys_find_extdev(dev_id, camsys_dev);
        if (extdev == NULL) {
            err = -EINVAL;
            camsys_warn("Extdev(dev_id: 0x%x) isn't registered in %s!",
                dev_id, dev_name(camsys_dev->miscdev.this_device));
            goto end;
        }

        regulator = &extdev->avdd;
        for (i=(CamSys_Vdd_Start_Tag+1); i<CamSys_Vdd_End_Tag; i++) {
            if (!IS_ERR_OR_NULL(regulator->ldo)) {
                while(regulator_is_enabled(regulator->ldo)>0)	
		            regulator_disable(regulator->ldo);
		        regulator_put(regulator->ldo);
            }
            regulator++;
        }

        gpio = &extdev->pwrdn;
        for (i=(CamSys_Gpio_Start_Tag+1); i<CamSys_Gpio_End_Tag; i++) {
            if (gpio->io!=0xffffffff) {                    
                gpio_free(gpio->io);
            }
            gpio++;
        }

        //spin_lock(&camsys_dev->lock);
        mutex_lock(&camsys_dev->extdevs.mut);
        list_del_init(&extdev->list);
        list_del_init(&extdev->active);
        //spin_unlock(&camsys_dev->lock);
        mutex_unlock(&camsys_dev->extdevs.mut);
        
        camsys_trace(1,"Extdev(dev_id: 0x%x) is deregister success", extdev->dev_id);
        kfree(extdev);
        extdev = NULL;
        
    } else {
        //spin_lock(&camsys_dev->lock);
        mutex_lock(&camsys_dev->extdevs.mut);
        while (!list_empty(&camsys_dev->extdevs.list)) {

            extdev = list_first_entry(&camsys_dev->extdevs.list, camsys_extdev_t, list);
            if (extdev) {
                regulator = &extdev->avdd;
                for (i=(CamSys_Vdd_Start_Tag+1); i<CamSys_Vdd_End_Tag; i++) {
                    if (!IS_ERR(regulator->ldo)) {
                        while(regulator_is_enabled(regulator->ldo)>0)	
    			            regulator_disable(regulator->ldo);
    			        regulator_put(regulator->ldo);
                    }
                    regulator++; 
                }

                gpio = &extdev->pwrdn;
                for (i=(CamSys_Gpio_Start_Tag+1); i<CamSys_Gpio_End_Tag; i++) {
                    if (gpio->io!=0xffffffff) {                    
                        gpio_free(gpio->io);
                    }
                    gpio++;
                }
                camsys_trace(1,"Extdev(dev_id: 0x%x) is deregister success", extdev->dev_id);
                list_del_init(&extdev->list);
                list_del_init(&extdev->active);
                kfree(extdev);
                extdev=NULL;
            }
        }
        //spin_unlock(&camsys_dev->lock);        
        mutex_unlock(&camsys_dev->extdevs.mut);
        camsys_trace(1, "All extdev is deregister success!");
    }
    

end:    
    return err;

}

static int camsys_sysctl(camsys_sysctrl_t *devctl, camsys_dev_t *camsys_dev)
{
    int i;
    int err = 0;    
    camsys_extdev_t *extdev,*extdev2;

    //spin_lock(&camsys_dev->lock);
    mutex_lock(&camsys_dev->extdevs.mut);
	if(devctl->ops == 0xaa){
		dump_stack();
		return 0;
	}
    //Internal 
    if (camsys_dev->dev_id & devctl->dev_mask) {
        switch (devctl->ops)
        {
            case CamSys_ClkIn:
            {
                camsys_dev->clkin_cb(camsys_dev,devctl->on);
                break;
            }

            case CamSys_Rst:
            {
                camsys_dev->reset_cb(camsys_dev, devctl->on);
                break;
            } 
            case CamSys_Flash_Trigger:
            {
                camsys_dev->flash_trigger_cb(camsys_dev, devctl->on);
                break;
            }
            case CamSys_IOMMU:
            {
                if(camsys_dev->iommu_cb(camsys_dev, devctl) < 0){
                    err = -1;
                    }
                break;
            }
            default:
                break;

        }
    }

    //External
    for (i=0; i<8; i++) {
        if (devctl->dev_mask & (1<<(i+24))) {
            extdev = camsys_find_extdev((1<<(i+24)), camsys_dev);
            if (extdev) {
                camsys_sysctl_extdev(extdev, devctl, camsys_dev);

                if (devctl->ops == CamSys_ClkIn) {
                    if (devctl->on) {
                        list_add_tail(&extdev->active,&camsys_dev->extdevs.active);
                    } else {
                        if (!list_empty(&camsys_dev->extdevs.active)) {    /* ddla@rock-chips.com: v0.0.7 */
                            list_for_each_entry(extdev2, &camsys_dev->extdevs.active, active) {
                                if (extdev2 == extdev) {
                                    list_del_init(&extdev->active);
                                    break;
                                }
                            }
                        }
                    }
                }
                
            } else {
                camsys_err("Can not find dev_id 0x%x device in %s!", (1<<(i+24)), dev_name(camsys_dev->miscdev.this_device));
            }
        }
    }

    //spin_unlock(&camsys_dev->lock);
    mutex_unlock(&camsys_dev->extdevs.mut);
    return err;
}
static int camsys_phy_ops (camsys_extdev_t *extdev, camsys_sysctrl_t *devctl, void *ptr)
{
    camsys_dev_t *camsys_dev = (camsys_dev_t*)ptr;
    camsys_mipiphy_t *mipiphy;
    int err = 0;
    
    if (extdev->phy.type == CamSys_Phy_Mipi) {
        mipiphy = (camsys_mipiphy_t*)devctl->rev;
        if (devctl->on == 0) {
            mipiphy->phy_index = extdev->phy.info.mipi.phy_index;
            mipiphy->bit_rate = 0;
            mipiphy->data_en_bit = 0x00;
        } else {
            if ((mipiphy->bit_rate == 0) || (mipiphy->data_en_bit == 0)) {
                *mipiphy = extdev->phy.info.mipi;
            }
            if (mipiphy->phy_index != extdev->phy.info.mipi.phy_index) {
                camsys_warn("mipiphy->phy_index(%d) != extdev->phy.info.mipi.phy_index(%d)!",
                    mipiphy->phy_index,extdev->phy.info.mipi.phy_index);
                mipiphy->phy_index = extdev->phy.info.mipi.phy_index;
                
            }
        }
        err = camsys_dev->mipiphy[mipiphy->phy_index].ops(ptr,mipiphy);
        if (err < 0) {
            camsys_err("extdev(0x%x) mipi phy ops config failed!",extdev->dev_id);
        }
    }

    return err;
}
static int camsys_irq_connect(camsys_irqcnnt_t *irqcnnt, camsys_dev_t *camsys_dev)
{
    int err = 0,i;
    camsys_irqpool_t *irqpool; 
    unsigned long int flags;

    if ((irqcnnt->mis != MRV_ISP_MIS) &&
        (irqcnnt->mis != MRV_MIPI_MIS) &&
        (irqcnnt->mis != MRV_MI_MIS) &&
        (irqcnnt->mis != MRV_JPG_MIS) &&
        (irqcnnt->mis != MRV_JPG_ERR_MIS)) {

        camsys_err("this thread(pid: %d) irqcnnt->mis(0x%x) is invalidate, irq connect failed!",
            irqcnnt->pid, irqcnnt->mis);

        err = -EINVAL;
        goto end;
    }   

    spin_lock_irqsave(&camsys_dev->irq.lock,flags);
    if (!list_empty(&camsys_dev->irq.irq_pool)) {
        list_for_each_entry(irqpool, &camsys_dev->irq.irq_pool, list) {
            if (irqpool->pid == irqcnnt->pid) {
                camsys_warn("this thread(pid: %d) had been connect irq!",current->pid);
                spin_unlock(&camsys_dev->irq.lock);
                goto end;
            }
        }
    }
    spin_unlock_irqrestore(&camsys_dev->irq.lock,flags);
    
    irqpool = kzalloc(sizeof(camsys_irqpool_t),GFP_KERNEL);
    if (irqpool) {
        spin_lock_init(&irqpool->lock);
        irqpool->pid = irqcnnt->pid;
        irqpool->timeout = irqcnnt->timeout;
        irqpool->mis = irqcnnt->mis;
        irqpool->icr = irqcnnt->icr;
        INIT_LIST_HEAD(&irqpool->active);
        INIT_LIST_HEAD(&irqpool->deactive);
        init_waitqueue_head(&irqpool->done);
        for (i=0; i<CAMSYS_IRQPOOL_NUM; i++) {
            list_add_tail(&irqpool->pool[i].list, &irqpool->deactive);
        }
    }
    
    spin_lock_irqsave(&camsys_dev->irq.lock,flags);
    //camsys_dev->irq.timeout = irqcnnt->timeout;
    list_add_tail(&irqpool->list, &camsys_dev->irq.irq_pool);
    spin_unlock_irqrestore(&camsys_dev->irq.lock,flags);
    camsys_trace(1, "Thread(pid: %d) connect %s irq success! mis: 0x%x icr: 0x%x ", irqpool->pid, dev_name(camsys_dev->miscdev.this_device),
        irqpool->mis,irqpool->icr);

end:
    return err;
}
static int active_list_isnot_empty(camsys_irqpool_t *irqpool)
{
    int err;
    unsigned long int flags;
    
    spin_lock_irqsave(&irqpool->lock,flags);
    err = list_empty(&irqpool->active);
    spin_unlock_irqrestore(&irqpool->lock,flags);

    return !err;
    
}
static int camsys_irq_wait(camsys_irqsta_t *irqsta, camsys_dev_t *camsys_dev)
{
    int err = 0;
    bool find_pool = false;
    camsys_irqstas_t *irqstas;
    camsys_irqpool_t *irqpool;
    unsigned long int flags;
    
    spin_lock_irqsave(&camsys_dev->irq.lock,flags);
    if (!list_empty(&camsys_dev->irq.irq_pool)) {
        list_for_each_entry(irqpool, &camsys_dev->irq.irq_pool, list) {
            if (irqpool->pid == current->pid) {
                find_pool = true;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&camsys_dev->irq.lock,flags);

    if (find_pool == false) {
        camsys_err("this thread(pid: %d) hasn't been connect irq, so wait irq failed!",current->pid);
        err = -EINVAL;
        goto end;
    }
    
    
    spin_lock_irqsave(&irqpool->lock,flags);
    if (!list_empty(&irqpool->active)) {
        irqstas = list_first_entry(&irqpool->active, camsys_irqstas_t, list);
        *irqsta = irqstas->sta;
        list_del_init(&irqstas->list);
        list_add_tail(&irqstas->list,&irqpool->deactive);
        spin_unlock_irqrestore(&irqpool->lock,flags);
    } else {
        spin_unlock_irqrestore(&irqpool->lock,flags);
        
        wait_event_interruptible_timeout(irqpool->done,
            active_list_isnot_empty(irqpool),
            usecs_to_jiffies(irqpool->timeout));

        if (irqpool->pid == current->pid) {
            if (active_list_isnot_empty(irqpool)) {
                spin_lock_irqsave(&irqpool->lock,flags);
                irqstas = list_first_entry(&irqpool->active, camsys_irqstas_t, list);
                *irqsta = irqstas->sta;
                list_del_init(&irqstas->list);
                list_add_tail(&irqstas->list,&irqpool->deactive);
                spin_unlock_irqrestore(&irqpool->lock,flags);
            } else {
                err = -EAGAIN;
            }
        } else {
            camsys_warn("Thread(pid: %d) has been disconnect!",current->pid);
            err = -EAGAIN;
        }
    }

    if (err == 0) {
        camsys_trace(3,"Thread(pid: %d) has been wake up for irq(mis: 0x%x ris:0x%x)!",
                     current->pid, irqsta->mis, irqsta->ris);
    }

end:
    return err;
}

static int camsys_irq_disconnect(camsys_irqcnnt_t *irqcnnt, camsys_dev_t *camsys_dev, bool all)
{
    int err = 0;
    bool find_pool = false;
    camsys_irqpool_t *irqpool;    
    unsigned long int flags;
    
    if (all == false) {
        spin_lock_irqsave(&camsys_dev->irq.lock,flags);
		if (!list_empty(&camsys_dev->irq.irq_pool)) {
            list_for_each_entry(irqpool, &camsys_dev->irq.irq_pool, list) {
                if (irqpool->pid == irqcnnt->pid) {
                    find_pool = true;
                    irqpool->pid = 0;
                    break;
                }
            }
        }
        spin_unlock_irqrestore(&camsys_dev->irq.lock,flags);

        if (find_pool == false) {
            camsys_err("this thread(pid: %d) have not been connect irq!, disconnect failed",current->pid);         
        } else {
            wake_up_all(&irqpool->done);
        }

        camsys_trace(1, "Thread(pid: %d) disconnect %s irq success!", irqcnnt->pid, dev_name(camsys_dev->miscdev.this_device));
   } else {
        spin_lock_irqsave(&camsys_dev->irq.lock,flags);
        while (!list_empty(&camsys_dev->irq.irq_pool)) {
            irqpool = list_first_entry(&camsys_dev->irq.irq_pool, camsys_irqpool_t, list);
            list_del_init(&irqpool->list);
            irqpool->pid = 0;
            wake_up_all(&irqpool->done);
            kfree(irqpool);
            irqpool = NULL;
        }
        spin_unlock_irqrestore(&camsys_dev->irq.lock,flags);

        camsys_trace(1, "All thread disconnect %s irq success!", dev_name(camsys_dev->miscdev.this_device));
   }


    return err;
}

static int camsys_querymem (camsys_dev_t *camsys_dev,  camsys_querymem_t *qmem)
{
    int err = 0;
    
    if (qmem->mem_type == CamSys_Mmap_RegisterMem) {
        if (camsys_dev->devmems.registermem == NULL) {
            camsys_err("%s register memory isn't been register!", dev_name(camsys_dev->miscdev.this_device));
            err = -EINVAL;
            goto end;
        }

        qmem->mem_size = camsys_dev->devmems.registermem->size;
        qmem->mem_offset = CamSys_Mmap_RegisterMem*PAGE_SIZE;
    } else if (qmem->mem_type == CamSys_Mmap_I2cMem) {
        if (camsys_dev->devmems.i2cmem== NULL) {
            camsys_err("%s i2c memory isn't been register!", dev_name(camsys_dev->miscdev.this_device));
            err = -EINVAL;
            goto end;
        }

        qmem->mem_size = camsys_dev->devmems.i2cmem->size;
        qmem->mem_offset = CamSys_Mmap_I2cMem*PAGE_SIZE;
    } else {
        camsys_err("%d memory type have not in %s memory list",qmem->mem_type,dev_name(camsys_dev->miscdev.this_device));
        err = -EINVAL;
        goto end;
    }
    

    return 0;
end: 
    return err;
}
static int camsys_open(struct inode *inode, struct file *file)
{
    int err = 0;
    int minor = iminor(inode);
    camsys_dev_t *camsys_dev;
    unsigned int i,phycnt;

    spin_lock(&camsys_devs.lock);
    list_for_each_entry(camsys_dev, &camsys_devs.devs, list) {
        if (camsys_dev->miscdev.minor == minor) {
            file->private_data = (void*)(camsys_dev);
            break;
        }
    }
    spin_unlock(&camsys_devs.lock);

    //zyc add
    INIT_LIST_HEAD(&camsys_dev->extdevs.active);
    
    if (camsys_dev->mipiphy != NULL) {
        phycnt = camsys_dev->mipiphy[0].phycnt;
         
        for (i=0; i<phycnt; i++) {
            if (camsys_dev->mipiphy[i].clkin_cb != NULL) {
                camsys_dev->mipiphy[i].clkin_cb(camsys_dev,1);
            }
        }
    }

    
    if (file->private_data == NULL) {
        camsys_err("Cann't find camsys_dev!");
        err = -ENODEV;
        goto end;
    } else {     
        camsys_trace(1,"%s(%p) is opened!",dev_name(camsys_dev->miscdev.this_device),camsys_dev);
    }

end:
    return err;
}

static int camsys_release(struct inode *inode, struct file *file)
{
    camsys_dev_t *camsys_dev = (camsys_dev_t*)file->private_data;
    unsigned int i,phycnt;
    
    camsys_irq_disconnect(NULL,camsys_dev, true);

    if (camsys_dev->mipiphy != NULL) {
        phycnt = camsys_dev->mipiphy[0].phycnt;
         
        for (i=0; i<phycnt; i++) {
            if (camsys_dev->mipiphy[i].clkin_cb != NULL) {
                camsys_dev->mipiphy[i].clkin_cb(camsys_dev,0);
            }
        }
    }

    camsys_trace(1,"%s(%p) is closed",dev_name(camsys_dev->miscdev.this_device),camsys_dev);

    return 0;
}

/*
* The ioctl() implementation
*/

static long camsys_ioctl(struct file *filp,unsigned int cmd, unsigned long arg)
{
	long err = 0;
    camsys_dev_t *camsys_dev = (camsys_dev_t*)filp->private_data; 
    
	if (_IOC_TYPE(cmd) != CAMSYS_IOC_MAGIC) { 
        camsys_err("ioctl type(%c!=%c) is invalidate\n",_IOC_TYPE(cmd),CAMSYS_IOC_MAGIC);
        err = -ENOTTY;
        goto end;
	}
	if (_IOC_NR(cmd) > CAMSYS_IOC_MAXNR) {
        camsys_err("ioctl index(%d>%d) is invalidate\n",_IOC_NR(cmd),CAMSYS_IOC_MAXNR);
        err = -ENOTTY;
        goto end;
	}

    if (_IOC_DIR(cmd) & _IOC_READ)
        err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));	
    else if (_IOC_DIR(cmd) & _IOC_WRITE)
        err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));

    if (err) {
        camsys_err("ioctl(0x%x) operation not permitted for %s",cmd,dev_name(camsys_dev->miscdev.this_device));
        err = -EFAULT;
        goto end;
    }

	switch (cmd) {

	    case CAMSYS_VERCHK:
	    {
	        camsys_version_t camsys_ver;
	        
            camsys_ver.drv_ver = CAMSYS_DRIVER_VERSION;
            camsys_ver.head_ver = CAMSYS_HEAD_VERSION;
            if (copy_to_user((void __user *)arg,(void*)&camsys_ver, sizeof(camsys_version_t)))
                return -EFAULT;
            break;
	    }
	    case CAMSYS_QUREYIOMMU:
	    {
            int iommu_enabled = 0;
            #ifdef CONFIG_ROCKCHIP_IOMMU
				struct device_node * vpu_node =NULL;
				int vpu_iommu_enabled = 0;
                vpu_node = of_find_compatible_node(NULL,NULL, "vpu_service");
				if(vpu_node){
					of_property_read_u32(vpu_node, "iommu_enabled", &vpu_iommu_enabled);
					of_property_read_u32(camsys_dev->pdev->dev.of_node, "rockchip,isp,iommu_enable", &iommu_enabled);
					if(iommu_enabled != vpu_iommu_enabled){
						camsys_err("iommu status not consistent,check the dts file ! isp:%d,vpu:%d",iommu_enabled,vpu_iommu_enabled);
						return -EFAULT;
					}
				}
			#endif
            if (copy_to_user((void __user *)arg,(void*)&iommu_enabled, sizeof(iommu_enabled)))
                return -EFAULT;
            break;
	    }
	    case CAMSYS_I2CRD:
	    {
	        camsys_i2c_info_t i2cinfo;
	        
            if (copy_from_user((void*)&i2cinfo,(void __user *)arg, sizeof(camsys_i2c_info_t))) 
                return -EFAULT;

            err = camsys_i2c_read(&i2cinfo,camsys_dev);
            if (err==0) {
                if (copy_to_user((void __user *)arg,(void*)&i2cinfo, sizeof(camsys_i2c_info_t)))
                    return -EFAULT;
            }
            break;
	    }

	    case CAMSYS_I2CWR:
	    {
            camsys_i2c_info_t i2cinfo;
	        
            if (copy_from_user((void*)&i2cinfo,(void __user *)arg, sizeof(camsys_i2c_info_t))) 
                return -EFAULT;

            err = camsys_i2c_write(&i2cinfo,camsys_dev);
            break;
	    }

        case CAMSYS_SYSCTRL:
        {
            camsys_sysctrl_t devctl;

            if (copy_from_user((void*)&devctl,(void __user *)arg, sizeof(camsys_sysctrl_t))) 
                return -EFAULT;

            err = camsys_sysctl(&devctl, camsys_dev);
            if ((err==0) && (devctl.ops == CamSys_IOMMU)){
                if (copy_to_user((void __user *)arg,(void*)&devctl, sizeof(camsys_sysctrl_t))) 
                    return -EFAULT;
            }
            break;
        }

        case CAMSYS_REGRD:
        {

            break;
        }

        case CAMSYS_REGWR:
        {

            break;
        }

        case CAMSYS_REGISTER_DEVIO:
        {
            camsys_devio_name_t devio;

            if (copy_from_user((void*)&devio,(void __user *)arg, sizeof(camsys_devio_name_t))) 
                return -EFAULT;

            err = camsys_extdev_register(&devio,camsys_dev);
            break;
        }

        case CAMSYS_DEREGISTER_DEVIO:
        {
            unsigned int dev_id;

            if (copy_from_user((void*)&dev_id,(void __user *)arg, sizeof(unsigned int)))
                return -EFAULT;

            err = camsys_extdev_deregister(dev_id, camsys_dev, false);
            break;
        }

        case CAMSYS_IRQCONNECT:
        {
            camsys_irqcnnt_t irqcnnt;

            if (copy_from_user((void*)&irqcnnt,(void __user *)arg, sizeof(camsys_irqcnnt_t))) 
                return -EFAULT;
            
            err = camsys_irq_connect(&irqcnnt, camsys_dev);
            
            break;
        }

        case CAMSYS_IRQWAIT:
        {
            camsys_irqsta_t irqsta;

            err = camsys_irq_wait(&irqsta, camsys_dev);
            if (err==0) {
                if (copy_to_user((void __user *)arg,(void*)&irqsta, sizeof(camsys_irqsta_t))) 
                    return -EFAULT;
            }
            break;
        }

        case CAMSYS_IRQDISCONNECT:
        {
            camsys_irqcnnt_t irqcnnt;

            if (copy_from_user((void*)&irqcnnt,(void __user *)arg, sizeof(camsys_irqcnnt_t))) 
                return -EFAULT;
            err = camsys_irq_disconnect(&irqcnnt,camsys_dev,false);
			break;
        }

        
        case CAMSYS_QUREYMEM:
        {
            camsys_querymem_t qmem;

            if (copy_from_user((void*)&qmem,(void __user *)arg, sizeof(camsys_querymem_t))) 
                return -EFAULT;
            
            err = camsys_querymem(camsys_dev,&qmem);
            if (err == 0) {
                if (copy_to_user((void __user *)arg,(void*)&qmem, sizeof(camsys_querymem_t))) 
                    return -EFAULT;
            }
            break;
        }
       
        default :
            break;
	}

end:	
	return err;

}
/*
 * VMA operations.
 */
static void camsys_vm_open(struct vm_area_struct *vma)
{
    camsys_meminfo_t *meminfo = (camsys_meminfo_t*)vma->vm_private_data;

    meminfo->vmas++;
    return;
}

static void camsys_vm_close(struct vm_area_struct *vma)
{
    camsys_meminfo_t *meminfo = (camsys_meminfo_t*)vma->vm_private_data;

    meminfo->vmas--;
    return;
}

static const struct vm_operations_struct camsys_vm_ops = {
	.open		= camsys_vm_open,
	.close		= camsys_vm_close,
};

int camsys_mmap(struct file *flip, struct vm_area_struct *vma)
{
    camsys_dev_t *camsys_dev = (camsys_dev_t*)flip->private_data;
	unsigned long addr, start, size;    
	camsys_mmap_type_t mem_type;
	camsys_meminfo_t *meminfo;		
	int ret = 0;

    mem_type = vma->vm_pgoff;     
    
    if (mem_type == CamSys_Mmap_RegisterMem) {
        if (camsys_dev->devmems.registermem != NULL) {
            meminfo = camsys_dev->devmems.registermem;
        } else {
            camsys_err("this camsys device has not register mem!");
            ret = -EINVAL;
            goto done;
        }
    } else if (mem_type == CamSys_Mmap_I2cMem) {
        if (camsys_dev->devmems.i2cmem != NULL) {
            meminfo = camsys_dev->devmems.i2cmem;
        } else {
            camsys_err("this camsys device has not i2c mem!");
            ret = -EINVAL;
            goto done;
        }
    } else {
        camsys_err("mmap buffer type %d is invalidate!",mem_type);
        ret = -EINVAL;
        goto done;
    }
    
    size = vma->vm_end - vma->vm_start;
    if (size > meminfo->size) {
        ret = -ENOMEM;
        camsys_err("mmap size(0x%lx) > memory size(0x%x), so failed!",size,meminfo->size);
        goto done;
    }
    
	start = vma->vm_start;
	addr = __phys_to_pfn(meminfo->phy_base);    
	
    if (remap_pfn_range(vma, start, addr,size,pgprot_noncached(vma->vm_page_prot))) { 
        
        ret = -EAGAIN;
        goto done;
    }
    
    vma->vm_ops = &camsys_vm_ops;
    vma->vm_flags |= VM_IO;
    vma->vm_flags |=VM_ACCOUNT;//same as VM_RESERVED;
    vma->vm_private_data = (void*)meminfo;
    camsys_vm_open(vma);

done:
	return ret;
}

struct file_operations camsys_fops = {
	.owner =            THIS_MODULE,
    .open =             camsys_open,
    .release =          camsys_release,
	.unlocked_ioctl =   camsys_ioctl,
	.mmap =             camsys_mmap,
};

static int camsys_platform_probe(struct platform_device *pdev){
    int err = 0;
	camsys_dev_t *camsys_dev;
    struct resource register_res ;
    struct device *dev = &pdev->dev;
    unsigned long i2cmem;
	camsys_meminfo_t *meminfo;
    unsigned int irq_id;
    
    err = of_address_to_resource(dev->of_node, 0, &register_res);
    if (err < 0){
        camsys_err("Get register resource from %s platform device failed!",pdev->name);
        err = -ENODEV;
        goto fail_end;
    }

    //map irqs
    irq_id = irq_of_parse_and_map(dev->of_node, 0);
    if (irq_id < 0) {
        camsys_err("Get irq resource from %s platform device failed!",pdev->name);
        err = -ENODEV;
        goto fail_end;
    }

    camsys_dev = (camsys_dev_t*)devm_kzalloc(&pdev->dev,sizeof(camsys_dev_t), GFP_KERNEL);
    if (camsys_dev == NULL) {
        camsys_err("Allocate camsys_dev for %s platform device failed",pdev->name);
        err = -ENOMEM;
        goto fail_end;
    }

    enum of_gpio_flags flags;
    int cifpower_io;
    int io_ret;
    cifpower_io = of_get_named_gpio_flags(dev->of_node, "gpios-cifpower", 0, &flags);
    camsys_trace(1, "1-gpios-cifpower： gpio=%d", cifpower_io);
    if(gpio_is_valid(cifpower_io)){
         cifpower_io = of_get_named_gpio_flags(dev->of_node, "gpios-cifpower", 0, &flags);
         camsys_trace(1, "gpios-cifpower： gpio_request");
         io_ret = gpio_request(cifpower_io,"cifpower");
         camsys_trace(1, "1-gpios-cifpower： gpio_request=%d", io_ret);
         if(io_ret < 0){
            camsys_err("Request %s(%d) failed","cifpower", cifpower_io);
         }
         else{
                gpio_direction_output(cifpower_io, 1);
                gpio_set_value(cifpower_io, 1);
                camsys_trace(1, "gpios-cifpower： %d high", cifpower_io);
         }
    }

    //spin_lock_init(&camsys_dev->lock);
    mutex_init(&camsys_dev->extdevs.mut);
    INIT_LIST_HEAD(&camsys_dev->extdevs.list);
    INIT_LIST_HEAD(&camsys_dev->extdevs.active);
    INIT_LIST_HEAD(&camsys_dev->list);
    

    //IRQ init
    camsys_dev->irq.irq_id = irq_id;  
    spin_lock_init(&camsys_dev->irq.lock);
    INIT_LIST_HEAD(&camsys_dev->irq.irq_pool); 
    //init_waitqueue_head(&camsys_dev->irq.irq_done);
    
    INIT_LIST_HEAD(&camsys_dev->devmems.memslist);

    // get soc operation
    camsys_dev->soc = (void*)camsys_soc_get();
    if (camsys_dev->soc == NULL) {
        err = -ENODEV;
        goto fail_end;
    }

    //Register mem init
    meminfo = kzalloc(sizeof(camsys_meminfo_t),GFP_KERNEL);
    if (meminfo == NULL) {
        err = -ENOMEM;
        goto request_mem_fail;
    }

    meminfo->vir_base = (unsigned int)devm_ioremap_resource(dev, &register_res);
    if (!meminfo->vir_base){
        camsys_err("%s ioremap %s failed",dev_name(&pdev->dev), CAMSYS_REGISTER_MEM_NAME);
        err = -ENXIO;
        goto request_mem_fail;
    }

    strlcpy(meminfo->name, CAMSYS_REGISTER_MEM_NAME,sizeof(meminfo->name));
    meminfo->phy_base = register_res.start;
    meminfo->size = register_res.end - register_res.start + 1;  
    list_add_tail(&meminfo->list, &camsys_dev->devmems.memslist);


    //I2c mem init
    i2cmem = __get_free_page(GFP_KERNEL);
    if (i2cmem == 0) {
        camsys_err("Allocate i2cmem failed!");
        err = -ENOMEM;
        goto request_mem_fail;
    }
    SetPageReserved(virt_to_page(i2cmem));
    
    meminfo = kzalloc(sizeof(camsys_meminfo_t),GFP_KERNEL);
    if (meminfo == NULL) {
        err = -ENOMEM;
        goto request_mem_fail;
    }
    strlcpy(meminfo->name,CAMSYS_I2C_MEM_NAME,sizeof(meminfo->name));
    meminfo->vir_base = i2cmem;
    meminfo->phy_base = virt_to_phys((void*)i2cmem);
    meminfo->size = PAGE_SIZE;
    list_add_tail(&meminfo->list, &camsys_dev->devmems.memslist);

    {
        unsigned int *tmpp;

        tmpp = (unsigned int*)meminfo->vir_base;
        *tmpp = 0xfa561243;
    }

    //Special init

    {        
        if (camsys_mipiphy_probe_cb(pdev, camsys_dev) <0) {
            camsys_err("Mipi phy probe failed!");
        }
    }

#if 0
    if (strcmp(dev_name(&pdev->dev),CAMSYS_PLATFORM_MARVIN_NAME) == 0) {
        #if (defined(CONFIG_CAMSYS_MRV))
        camsys_mrv_probe_cb(pdev, camsys_dev);        
        #else
        camsys_err("Marvin controller camsys driver haven't been complie!!!");
        #endif
    } else {
        #if (defined(CONFIG_CAMSYS_CIF))
        camsys_cif_probe_cb(pdev,camsys_dev);
        #else
        camsys_err("CIF controller camsys driver haven't been complie!!!");
        #endif
    }
#else
        #if (defined(CONFIG_CAMSYS_MRV))
        camsys_mrv_probe_cb(pdev, camsys_dev);        
        #elif (defined(CONFIG_CAMSYS_CIF))
        camsys_cif_probe_cb(pdev,camsys_dev);
        #else
        camsys_err("camsys driver haven't been complie!!!");
        #endif
#endif
    camsys_trace(1, "%s memory:",dev_name(&pdev->dev));
    list_for_each_entry(meminfo, &camsys_dev->devmems.memslist, list) {
        if (strcmp(meminfo->name,CAMSYS_I2C_MEM_NAME) == 0) {
            camsys_dev->devmems.i2cmem = meminfo;
            camsys_trace(1,"    I2c memory (phy: 0x%x vir: 0x%x size: 0x%x)",
                        meminfo->phy_base,meminfo->vir_base,meminfo->size);
        }
        if (strcmp(meminfo->name,CAMSYS_REGISTER_MEM_NAME) == 0) {
            camsys_dev->devmems.registermem = meminfo;
            camsys_trace(1,"    Register memory (phy: 0x%x vir: 0x%x size: 0x%x)",
                        meminfo->phy_base,meminfo->vir_base,meminfo->size);
        }
    }


    camsys_dev->phy_cb = camsys_phy_ops;
    camsys_dev->pdev    =   pdev;

    platform_set_drvdata(pdev,(void*)camsys_dev);
    //Camsys_devs list add    
    spin_lock(&camsys_devs.lock);    
    list_add_tail(&camsys_dev->list, &camsys_devs.devs);
    spin_unlock(&camsys_devs.lock);

    
    camsys_trace(1, "Probe %s device success ", dev_name(&pdev->dev));
    return 0;
request_mem_fail:
    if (camsys_dev != NULL) {
    
        while(!list_empty(&camsys_dev->devmems.memslist)) {
            meminfo = list_first_entry(&camsys_dev->devmems.memslist, camsys_meminfo_t, list);
            if (meminfo) {
                list_del_init(&meminfo->list);
                if (strcmp(meminfo->name,CAMSYS_REGISTER_MEM_NAME)==0) {
                    iounmap((void __iomem *)meminfo->vir_base);
                    release_mem_region(meminfo->phy_base,meminfo->size);
                } else if (strcmp(meminfo->name,CAMSYS_I2C_MEM_NAME)==0) {
                    kfree((void*)meminfo->vir_base);
                }
                kfree(meminfo);
                meminfo = NULL;
            }
        } 
    
        kfree(camsys_dev);
        camsys_dev = NULL;
    }
fail_end:
    return -1;

    
}
static int  camsys_platform_remove(struct platform_device *pdev)
{
    camsys_dev_t *camsys_dev = platform_get_drvdata(pdev);
    camsys_meminfo_t *meminfo;
    
    if (camsys_dev) {

        //Mem deinit
        while(!list_empty(&camsys_dev->devmems.memslist)) {
            meminfo = list_first_entry(&camsys_dev->devmems.memslist, camsys_meminfo_t, list);
            if (meminfo) {
                list_del_init(&meminfo->list);
                if (strcmp(meminfo->name,CAMSYS_REGISTER_MEM_NAME)==0) {
                    iounmap((void __iomem *)meminfo->vir_base);
                    release_mem_region(meminfo->phy_base,meminfo->size);
                } else if (strcmp(meminfo->name,CAMSYS_I2C_MEM_NAME)==0) {
                    kfree((void*)meminfo->vir_base);
                }
                kfree(meminfo);
                meminfo = NULL;
            }
        }        

        //Irq deinit
        if (camsys_dev->irq.irq_id) {
            free_irq(camsys_dev->irq.irq_id, camsys_dev);
            camsys_irq_disconnect(NULL,camsys_dev,true);
        }

        //Extdev deinit
        if (!list_empty(&camsys_dev->extdevs.list)) {
            camsys_extdev_deregister(0,camsys_dev,true);
        }
        if (camsys_dev->mipiphy != NULL) {
            camsys_dev->mipiphy->remove(pdev);
        }
        if (camsys_dev->cifphy.remove)
            camsys_dev->cifphy.remove(pdev);
        camsys_dev->platform_remove(pdev);

        misc_deregister(&camsys_dev->miscdev);
        
        spin_lock(&camsys_devs.lock);
        list_del_init(&camsys_dev->list);
        spin_unlock(&camsys_devs.lock);

        kfree(camsys_dev);
        camsys_dev=NULL;
    } else {
        camsys_err("This platform device havn't obtain camsys_dev!");
    }

    return 0;
}


static const struct of_device_id cif_of_match[] = {
    { .compatible = "rockchip,isp" },
};
MODULE_DEVICE_TABLE(of, cif_of_match);

static struct platform_driver camsys_platform_driver =
{
    .driver 	= {
        .name	= CAMSYS_PLATFORM_DRV_NAME,
        .of_match_table = of_match_ptr(cif_of_match),
    },
    .probe		= camsys_platform_probe,
    .remove		= (camsys_platform_remove),
};

MODULE_ALIAS(CAMSYS_PLATFORM_DRV_NAME);
static int __init camsys_platform_init(void)  
{
    printk("CamSys driver version: v%d.%d.%d,  CamSys head file version: v%d.%d.%d\n",
        (CAMSYS_DRIVER_VERSION&0xff0000)>>16, (CAMSYS_DRIVER_VERSION&0xff00)>>8,
        CAMSYS_DRIVER_VERSION&0xff,
        (CAMSYS_HEAD_VERSION&0xff0000)>>16, (CAMSYS_HEAD_VERSION&0xff00)>>8,
        CAMSYS_HEAD_VERSION&0xff);

    spin_lock_init(&camsys_devs.lock);
    INIT_LIST_HEAD(&camsys_devs.devs);
    camsys_soc_init();
    platform_driver_register(&camsys_platform_driver);
   // platform_driver_probe(&camsys_platform_driver, camsys_platform_probe_new);
    
    return 0;
}  
  
static void __exit camsys_platform_exit(void)  
{
    platform_driver_unregister(&camsys_platform_driver);
    camsys_soc_deinit();
} 

module_init(camsys_platform_init);		
module_exit(camsys_platform_exit);	

MODULE_DESCRIPTION("RockChip Camera System");
MODULE_AUTHOR("<ddl@rock-chips>");
MODULE_LICENSE("GPL");

