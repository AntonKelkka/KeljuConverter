/*
 * This file is part of the tumanako_vc project.
 *
 * Copyright (C) 2010 Johannes Huebner <contact@johanneshuebner.com>
 * Copyright (C) 2010 Edward Cheeseman <cheesemanedward@gmail.com>
 * Copyright (C) 2009 Uwe Hermann <uwe@hermann-uwe.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <libopencm3/cm3/common.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/rtc.h>
#include "hwdefs.h"
#include "hwinit.h"

/**
* Start clocks of all needed peripherals
*/
void clock_setup(void)
{
   RCC_CLOCK_SETUP();

   //The reset value for PRIGROUP (=0) is not actually a defined
   //value. Explicitly set 16 preemtion priorities
   SCB_AIRCR = SCB_AIRCR_VECTKEY | SCB_AIRCR_PRIGROUP_GROUP16_NOSUB;

   rcc_periph_clock_enable(RCC_GPIOA);
   rcc_periph_clock_enable(RCC_GPIOB);
   rcc_periph_clock_enable(RCC_GPIOC);
   rcc_periph_clock_enable(RCC_GPIOD);
   rcc_periph_clock_enable(RCC_USART3);
   rcc_periph_clock_enable(RCC_TIM1); //Main PWM
   rcc_periph_clock_enable(RCC_TIM2); //Scheduler, over current on blue pill
   rcc_periph_clock_enable(RCC_TIM3); //Rotor Encoder
   rcc_periph_clock_enable(RCC_TIM4); //Overcurrent / AUX PWM, scheduler on blue pill
   rcc_periph_clock_enable(RCC_DMA1);  //ADC, Encoder and UART receive
   rcc_periph_clock_enable(RCC_ADC1);
   rcc_periph_clock_enable(RCC_CRC);
   rcc_periph_clock_enable(RCC_AFIO); //CAN
   rcc_periph_clock_enable(RCC_CAN1); //CAN
}

/**
* Setup UART3 115200 8N1
*/
void usart_setup(void)
{
   gpio_set_mode(TERM_USART_TXPORT, GPIO_MODE_OUTPUT_50_MHZ,
               GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, TERM_USART_TXPIN);

   usart_set_baudrate(TERM_USART, USART_BAUDRATE);
   usart_set_databits(TERM_USART, 8);
   usart_set_stopbits(TERM_USART, USART_STOPBITS_2);
   usart_set_mode(TERM_USART, USART_MODE_TX_RX);
   usart_set_parity(TERM_USART, USART_PARITY_NONE);
   usart_set_flow_control(TERM_USART, USART_FLOWCONTROL_NONE);
   usart_enable_rx_dma(TERM_USART);

   usart_enable_tx_dma(TERM_USART);
   dma_channel_reset(DMA1, TERM_USART_DMATX);
   dma_set_read_from_memory(DMA1, TERM_USART_DMATX);
   dma_set_peripheral_address(DMA1, TERM_USART_DMATX, (uint32_t)&TERM_USART_DR);
   dma_set_peripheral_size(DMA1, TERM_USART_DMATX, DMA_CCR_PSIZE_8BIT);
   dma_set_memory_size(DMA1, TERM_USART_DMATX, DMA_CCR_MSIZE_8BIT);
   dma_enable_memory_increment_mode(DMA1, TERM_USART_DMATX);

   dma_channel_reset(DMA1, TERM_USART_DMARX);
   dma_set_peripheral_address(DMA1, TERM_USART_DMARX, (uint32_t)&TERM_USART_DR);
   dma_set_peripheral_size(DMA1, TERM_USART_DMARX, DMA_CCR_PSIZE_8BIT);
   dma_set_memory_size(DMA1, TERM_USART_DMARX, DMA_CCR_MSIZE_8BIT);
   dma_enable_memory_increment_mode(DMA1, TERM_USART_DMARX);
   dma_enable_channel(DMA1, TERM_USART_DMARX);

   usart_enable(TERM_USART);
}

/**
* Enable Timer refresh and break interrupts
*/
void nvic_setup(void)
{
   nvic_enable_irq(PWM_TIMER_IRQ); //Main PWM
   nvic_set_priority(PWM_TIMER_IRQ, 1 << 4); //Set second-highest priority

   nvic_enable_irq(NVIC_TIM1_BRK_IRQ); //Emergency shut down
   nvic_set_priority(NVIC_TIM1_BRK_IRQ, 0); //Highest priority

   nvic_enable_irq(NVIC_EXTI2_IRQ); //Encoder Index pulse
   nvic_set_priority(NVIC_EXTI2_IRQ, 0); //Set highest priority

   nvic_enable_irq(NVIC_TIM2_IRQ); //Scheduler
   nvic_set_priority(NVIC_TIM2_IRQ, 0xe << 4); //second lowest priority
}

void rtc_setup()
{
   //Base clock is HSE/128 = 8MHz/128 = 62.5kHz
   //62.5kHz / (624 + 1) = 100Hz
   rtc_auto_awake(RCC_HSE, 624); //10ms tick
   rtc_set_counter_val(0);
}

/**
* Setup main PWM timer and timer for generating over current
* reference values and external PWM
*
* @param[in] pwmdigits Number of binary digits to use (determines PWM frequency)
* @param[in] deadtime Deadtime between bottom and top (coded value, consult STM32 manual)
* @param[in] pwmpol Output Polarity. 0=Active High, 1=Active Low
* @return PWM frequency
*/
uint16_t tim_setup(uint16_t pwmdigits, int pwmpol)
{
   const uint16_t pwmmax = 1U << pwmdigits;
   /* disable timer */
   timer_disable_counter(PWM_TIMER);
   /* Center aligned PWM */
   timer_set_alignment(PWM_TIMER, TIM_CR1_CMS_CENTER_1);
   timer_enable_preload(PWM_TIMER);
   /* PWM mode 1 and preload enable */
   TIM_CCMR1(PWM_TIMER) = TIM_CCMR1_OC1M_PWM1 | TIM_CCMR1_OC1PE |
                        TIM_CCMR1_OC2M_PWM1 | TIM_CCMR1_OC2PE;
   TIM_CCMR2(PWM_TIMER) = TIM_CCMR2_OC3M_PWM1 | TIM_CCMR2_OC3PE;

   if (pwmpol)
   {
      timer_set_oc_polarity_low(PWM_TIMER, TIM_OC1);
      timer_set_oc_polarity_low(PWM_TIMER, TIM_OC2);
      timer_set_oc_polarity_low(PWM_TIMER, TIM_OC3);
   }
   else
   {
      timer_set_oc_polarity_high(PWM_TIMER, TIM_OC1);
      timer_set_oc_polarity_high(PWM_TIMER, TIM_OC2);
      timer_set_oc_polarity_high(PWM_TIMER, TIM_OC3);
   }

   //tim_output_enable();

   timer_disable_break_automatic_output(PWM_TIMER);
   timer_enable_break_main_output(PWM_TIMER);
   timer_set_break_polarity_high(PWM_TIMER);
   //timer_enable_break(PWM_TIMER);
   timer_set_enabled_off_state_in_run_mode(PWM_TIMER);
   timer_set_enabled_off_state_in_idle_mode(PWM_TIMER);

   timer_set_deadtime(PWM_TIMER, 0);

   timer_set_oc_idle_state_unset(PWM_TIMER, TIM_OC1);
   timer_set_oc_idle_state_unset(PWM_TIMER, TIM_OC2);
   timer_set_oc_idle_state_unset(PWM_TIMER, TIM_OC3);
   timer_set_oc_idle_state_unset(PWM_TIMER, TIM_OC1N);
   timer_set_oc_idle_state_unset(PWM_TIMER, TIM_OC2N);
   timer_set_oc_idle_state_unset(PWM_TIMER, TIM_OC3N);

   timer_generate_event(PWM_TIMER, TIM_EGR_UG);

   timer_clear_flag(PWM_TIMER, TIM_SR_BIF);
   timer_enable_irq(PWM_TIMER, TIM_DIER_UIE);
   timer_enable_irq(PWM_TIMER, TIM_DIER_BIE);

   timer_set_prescaler(PWM_TIMER, 0);
   /* PWM frequency */
   timer_set_period(PWM_TIMER, pwmmax);
   timer_set_repetition_counter(PWM_TIMER, 1);

   //timer_set_oc_value(PWM_TIMER, TIM_OC1, 0);
   //timer_set_oc_value(PWM_TIMER, TIM_OC2, 0);
   //timer_set_oc_value(PWM_TIMER, TIM_OC3, 0);

   //timer_enable_counter(PWM_TIMER);

   /*** Setup over/undercurrent and PWM output timer */
   timer_disable_counter(OVER_CUR_TIMER);
   //edge aligned PWM
   timer_set_alignment(OVER_CUR_TIMER, TIM_CR1_CMS_EDGE);
   timer_enable_preload(OVER_CUR_TIMER);
   /* PWM mode 1 and preload enable */
   timer_set_oc_mode(OVER_CUR_TIMER, TIM_OC2, TIM_OCM_PWM1);
   timer_set_oc_mode(OVER_CUR_TIMER, TIM_OC3, TIM_OCM_PWM1);
   timer_set_oc_mode(OVER_CUR_TIMER, TIM_OC4, TIM_OCM_PWM1);
   timer_enable_oc_preload(OVER_CUR_TIMER, TIM_OC2);
   timer_enable_oc_preload(OVER_CUR_TIMER, TIM_OC3);
   timer_enable_oc_preload(OVER_CUR_TIMER, TIM_OC4);

   timer_set_oc_polarity_high(OVER_CUR_TIMER, TIM_OC2);
   timer_set_oc_polarity_high(OVER_CUR_TIMER, TIM_OC3);
   timer_set_oc_polarity_high(OVER_CUR_TIMER, TIM_OC4);
   timer_enable_oc_output(OVER_CUR_TIMER, TIM_OC2);
   timer_enable_oc_output(OVER_CUR_TIMER, TIM_OC3);
   timer_enable_oc_output(OVER_CUR_TIMER, TIM_OC4);
   timer_generate_event(OVER_CUR_TIMER, TIM_EGR_UG);
   timer_set_prescaler(OVER_CUR_TIMER, 0);
   /* PWM frequency */
   timer_set_period(OVER_CUR_TIMER, OCURMAX);
   timer_enable_counter(OVER_CUR_TIMER);

   /** setup gpio */
   gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO8 | GPIO9 | GPIO10);
   gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO13 | GPIO14 | GPIO15 | GPIO7 | GPIO8 | GPIO9);

   return rcc_apb2_frequency / (uint32_t)pwmmax;
}

/**
* Enable timer PWM output
*/
void tim_output_enable()
{
   timer_enable_oc_output(PWM_TIMER, TIM_OC1);
   timer_enable_oc_output(PWM_TIMER, TIM_OC2);
   timer_enable_counter(PWM_TIMER);
}

/**
* Disable timer PWM output
*/
void tim_output_disable()
{
   timer_disable_oc_output(PWM_TIMER, TIM_OC1);
   timer_disable_oc_output(PWM_TIMER, TIM_OC2);
   timer_disable_oc_output(PWM_TIMER, TIM_OC3);
   timer_disable_oc_output(PWM_TIMER, TIM_OC1N);
   timer_disable_oc_output(PWM_TIMER, TIM_OC2N);
   timer_disable_oc_output(PWM_TIMER, TIM_OC3N);
   timer_disable_counter(PWM_TIMER);
}
