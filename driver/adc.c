/*
 * Open-BLDC - Open BrushLess DC Motor Controller
 * Copyright (C) 2011-2013 by Piotr Esden-Tempski <piotr@esden.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

/**
 * @file   adc.c
 * @author Piotr Esden-Tempski <piotr@esden.net>
 *
 * @brief  ADC driver implementation.
 *
 * Implements functions for initializing and controlling the adc subsystem.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/f1/gpio.h>
#include <libopencm3/stm32/f1/dma.h>
#include <libopencm3/stm32/f1/adc.h>
#include <libopencm3/stm32/f1/nvic.h>

#include "driver/adc.h"
#include "driver/led.h"

/* Define ADC channel gpio and channels. */

#define ADC_BANK GPIOA

#define ADC_BANK_U_VOLTAGE ADC_BANK
#define ADC_PORT_U_VOLTAGE GPIO0
#define ADC_CHAN_U_VOLTAGE 0

#define ADC_BANK_V_VOLTAGE ADC_BANK
#define ADC_PORT_V_VOLTAGE GPIO1
#define ADC_CHAN_V_VOLTAGE 1

#define ADC_BANK_W_VOLTAGE ADC_BANK
#define ADC_PORT_W_VOLTAGE GPIO2
#define ADC_CHAN_W_VOLTAGE 2

#define ADC_BANK_V_BATT ADC_BANK
#define ADC_PORT_V_BATT GPIO3
#define ADC_CHAN_V_BATT 3

#define ADC_BANK_CURRENT ADC_BANK
#define ADC_PORT_CURRENT GPIO4
#define ADC_CHAN_CURRENT 4

/* ADC configuration. */
#define ADC_SAMPLE_COUNT 8

static const uint8_t const adc1_channel_array[ADC_SAMPLE_COUNT] = {
	ADC_CHAN_U_VOLTAGE,
	ADC_CHAN_V_VOLTAGE,
	ADC_CHAN_W_VOLTAGE,
	ADC_CHAN_V_BATT,
	ADC_CHAN_U_VOLTAGE,
	ADC_CHAN_V_VOLTAGE,
	ADC_CHAN_W_VOLTAGE,
	ADC_CHAN_CURRENT
};

/* Define local state. */
struct adc_state {
	uint32_t raw_data[ADC_SAMPLE_COUNT];
} adc_state;

/**
 * Initialize analog to digital converter
 */
void adc_init(void) {
	/* Initialize peripheral clocks. */
	rcc_peripheral_enable_clock(&RCC_AHBENR, RCC_AHBENR_DMA1EN);
	rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_IOPAEN);
	rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_ADC1EN);
	rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_ADC2EN);

	/* Initialize the ADC input GPIO. */
	/* WARNING: this code is written to work with strip. On the strip hardware
	 * we are lucky and all the ADC channels are on the same bank so we can
	 * initialize all of them in one go. This code will need to be
	 * changed/improved if we ever have to support hardware that has the ADC's
	 * spread over more then one bank.
	 */
	gpio_set_mode(ADC_BANK, GPIO_MODE_INPUT,
				  GPIO_CNF_INPUT_ANALOG, ADC_PORT_U_VOLTAGE |
				  						 ADC_PORT_V_VOLTAGE |
				  						 ADC_PORT_W_VOLTAGE |
				  						 ADC_PORT_V_BATT |
				  						 ADC_PORT_CURRENT);

	/* Configure DMA for data aquisition. */
	dma_channel_reset(DMA1, DMA_CHANNEL1); /* Channel 1 reacts to: ADC1, TIM2_CH3 and TIM4_CH1 */

	dma_set_peripheral_address(DMA1, DMA_CHANNEL1, (uint32_t)&ADC1_DR);
	dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)adc_state.raw_data);
	dma_set_number_of_data(DMA1, DMA_CHANNEL1, ADC_SAMPLE_COUNT);
	dma_set_read_from_peripheral(DMA1, DMA_CHANNEL1);
	dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL1);
	dma_enable_circular_mode(DMA1, DMA_CHANNEL1);
	dma_set_peripheral_size(DMA1, DMA_CHANNEL1, DMA_CCR_PSIZE_32BIT);
	dma_set_memory_size(DMA1, DMA_CHANNEL1, DMA_CCR_MSIZE_32BIT);
	dma_set_priority(DMA1, DMA_CHANNEL1, DMA_CCR_PL_VERY_HIGH);

	dma_enable_half_transfer_interrupt(DMA1, DMA_CHANNEL1);
	dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL1);
	dma_enable_transfer_error_interrupt(DMA1, DMA_CHANNEL1);

	dma_enable_channel(DMA1, DMA_CHANNEL1);

	/* Configure interrupts in NVIC. */
	nvic_set_priority(NVIC_DMA1_CHANNEL1_IRQ, 0);
	nvic_enable_irq(NVIC_DMA1_CHANNEL1_IRQ);

	/* Disable ADC1. */
	adc_off(ADC1);

	/* Configure dualmode. */
	//adc_set_dual_mode(ADC_CR1_DUALMOD_RSM); /* Dualmode regular only. */

	/* Configure ADC1. */
	adc_enable_scan_mode(ADC1);
	adc_set_continuous_conversion_mode(ADC1);
	adc_set_right_aligned(ADC1);
	adc_enable_external_trigger_regular(ADC1, ADC_CR2_EXTSEL_SWSTART);
	adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_7DOT5CYC);
	adc_enable_dma(ADC1);

	adc_power_on(ADC1);

	{
		int i;
		for (i=0; i<800000; i++) /* Wait a bit for the adc to power on. */
			__asm("nop");
	}

	adc_reset_calibration(ADC1);
	adc_calibration(ADC1);

	adc_set_regular_sequence(ADC1, ADC_SAMPLE_COUNT, (uint8_t *)adc1_channel_array);

	adc_start_conversion_regular(ADC1);
}

void dma1_channel1_isr(void) {
	if (dma_get_interrupt_flag(DMA1, DMA_CHANNEL1, DMA_HTIF)) {
		ON(LED_RED);
	} 

	if (dma_get_interrupt_flag(DMA1, DMA_CHANNEL1, DMA_TCIF)) {
		OFF(LED_RED);
	}

	if (dma_get_interrupt_flag(DMA1, DMA_CHANNEL1, DMA_TEIF)) {
		TOGGLE(LED_GREEN);
	}

	dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_FLAGS);
}