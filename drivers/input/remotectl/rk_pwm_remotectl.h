
#ifndef __RKXX_PWM_REMOTECTL_H__
#define __RKXX_PWM_REMOTECTL_H__
#include <linux/input.h>

/* PWM0 registers  */
#define PWM_REG_CNTR                    0x00  /* Counter Register */
#define PWM_REG_HPR		                0x04  /* Period Register */
#define PWM_REG_LPR                     0x08  /* Duty Cycle Register */
#define PWM_REG_CTRL                    0x0c  /* Control Register */
#define PWM_REG_INTSTS                  0x40  /* Interrupt Status Refister */
#define PWM_REG_INT_EN                  0x44  /* Interrupt Enable Refister */

/*REG_CTRL bits definitions*/
#define PWM_ENABLE			            (1 << 0)
#define PWM_DISABLE			            (0 << 0)

/*operation mode*/
#define PWM_MODE_ONESHOT			    (0x00 << 1)
#define PWM_MODE_CONTINUMOUS 	        (0x01 << 1)
#define PWM_MODE_CAPTURE		        (0x02 << 1)

/*duty cycle output polarity*/
#define PWM_DUTY_POSTIVE	            (0x01 << 3)
#define PWM_DUTY_NEGATIVE	            (0x00 << 3)

/*incative state output polarity*/
#define PWM_INACTIVE_POSTIVE 		    (0x01 << 4)
#define PWM_INACTIVE_NEGATIVE		    (0x00 << 4)

/*clock source select*/
#define PWM_CLK_SCALE		            (1 << 9)
#define PWM_CLK_NON_SCALE 	            (0 << 9)

#define PWM_CH0_INT                     (1 << 0)
#define PWM_CH1_INT                     (1 << 1)
#define PWM_CH2_INT                     (1 << 2)
#define PWM_CH3_INT                     (1 << 3)

#define PWM_CH0_POL                     (1 << 8)
#define PWM_CH1_POL                     (1 << 9)
#define PWM_CH2_POL                     (1 << 10)
#define PWM_CH3_POL                     (1 << 11)

#define PWM_CH0_INT_ENABLE              (1 << 0)
#define PWM_CH0_INT_DISABLE             (0 << 0)

#define PWM_CH1_INT_ENABLE              (1 << 0)
#define PWM_CH1_INT_DISABLE             (0 << 1)

#define PWM_CH2_INT_ENABLE              (1 << 2)
#define PWM_CH2_INT_DISABLE             (0 << 2)

#define PWM_CH3_INT_ENABLE              (1 << 3)
#define PWM_CH3_INT_DISABLE             (0 << 3)

/*prescale factor*/
#define PWMCR_MIN_PRESCALE	            0x00
#define PWMCR_MAX_PRESCALE	            0x07

#define PWMDCR_MIN_DUTY	       	        0x0001
#define PWMDCR_MAX_DUTY		            0xFFFF

#define PWMPCR_MIN_PERIOD		        0x0001
#define PWMPCR_MAX_PERIOD		        0xFFFF

#define PWMPCR_MIN_PERIOD		        0x0001
#define PWMPCR_MAX_PERIOD		        0xFFFF

enum pwm_div {
        PWM_DIV1                 = (0x0 << 12),
        PWM_DIV2                 = (0x1 << 12),
        PWM_DIV4                 = (0x2 << 12),
        PWM_DIV8                 = (0x3 << 12),
        PWM_DIV16                = (0x4 << 12),
        PWM_DIV32                = (0x5 << 12),
        PWM_DIV64                = (0x6 << 12),
        PWM_DIV128   	         = (0x7 << 12),
}; 




/********************************************************************
**                            宏定义                                *
********************************************************************/
#define RK_PWM_TIME_PRE_MIN      19   /*4500*/
#define RK_PWM_TIME_PRE_MAX      30   /*5500*/           /*PreLoad 4.5+0.56 = 5.06ms*/

#define RK_PWM_TIME_BIT0_MIN     1  /*Bit0  1.125ms*/
#define RK_PWM_TIME_BIT0_MAX     5

#define RK_PWM_TIME_BIT1_MIN     7  /*Bit1  2.25ms*/
#define RK_PWM_TIME_BIT1_MAX     14

#define RK_PWM_TIME_RPT_MIN      0x215   /*101000*/
#define RK_PWM_TIME_RPT_MAX      0x235   /*103000*/         /*Repeat  105-2.81=102.19ms*/  //110-9-2.25-0.56=98.19ms

#define RK_PWM_TIME_SEQ1_MIN     2   /*2650*/
#define RK_PWM_TIME_SEQ1_MAX     0x20   /*3000*/           /*sequence  2.25+0.56=2.81ms*/ //11.25ms

#define RK_PWM_TIME_SEQ2_MIN     0xDE   /*101000*/
#define RK_PWM_TIME_SEQ2_MAX     0x120   /*103000*/         /*Repeat  105-2.81=102.19ms*/  //110-9-2.25-0.56=98.19ms

/********************************************************************
**                          结构定义                                *
********************************************************************/
typedef enum _RMC_STATE
{
    RMC_IDLE,
    RMC_PRELOAD,
    RMC_USERCODE,
    RMC_GETDATA,
    RMC_SEQUENCE
}eRMC_STATE;


struct RKxx_remotectl_platform_data {
	//struct rkxx_remotectl_button *buttons;
	int nbuttons;
	int rep;
	int timer;
	int wakeup;
};

#endif

