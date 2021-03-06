/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "test_aio.h"

static int cb_cnt;

static void callback(void *param)
{
	cb_cnt++;
}

static void set_aio_callback(int polarity, int disable)
{
	struct device *aio_dev = device_get_binding(AIO_CMP_DEV_NAME);

	struct device *gpio_dev = device_get_binding(GPIO_DEV_NAME);

	gpio_pin_configure(gpio_dev, PIN_OUT, GPIO_DIR_OUT);

	/* config AIN callback */
	assert_true(aio_cmp_configure(aio_dev, PIN_IN,
				      polarity, AIO_CMP_REF_A,
				      callback, (void *)aio_dev) == 0,
		    "ERROR registering callback");
	if (disable == 1) {
		aio_cmp_disable(aio_dev, PIN_IN);
	}

	/* update the AIN input to trigger state */
	gpio_pin_write(gpio_dev, PIN_OUT,
		       (polarity == AIO_CMP_POL_RISE) ? 0 : 1);
	k_sleep(100);
	cb_cnt = 0;
	gpio_pin_write(gpio_dev, PIN_OUT,
		       (polarity == AIO_CMP_POL_RISE) ? 1 : 0);

	k_sleep(1000);
	TC_PRINT("... cb_cnt = %d\n", cb_cnt);
}

/* export test cases */
void test_aio_callback_rise(void)
{
	set_aio_callback(AIO_CMP_POL_RISE, 0);
	assert_true(cb_cnt == 1, "callback is not invoked correctly");
}

void test_aio_callback_rise_disable(void)
{
	set_aio_callback(AIO_CMP_POL_RISE, 1);
	assert_true(cb_cnt == 0, "callback is not invoked correctly");
}
