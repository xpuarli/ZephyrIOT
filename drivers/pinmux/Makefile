ccflags-y +=-I$(srctree)/drivers

# Board initialization
obj-$(CONFIG_PINMUX_MCUX) += pinmux_mcux.o
obj-$(CONFIG_PINMUX_STM32) += stm32/pinmux_stm32.o
obj-$(CONFIG_PINMUX_BEETLE) += beetle/pinmux_board_v2m_beetle.o
obj-$(CONFIG_BOARD_NUCLEO_F103RB) += stm32/pinmux_board_nucleo_f103rb.o
obj-$(CONFIG_BOARD_NUCLEO_F401RE) += stm32/pinmux_board_nucleo_f401re.o
obj-$(CONFIG_BOARD_NUCLEO_F411RE) += stm32/pinmux_board_nucleo_f411re.o
obj-$(CONFIG_BOARD_96B_CARBON) += stm32/pinmux_board_carbon.o
obj-$(CONFIG_BOARD_NUCLEO_L476RG) += stm32/pinmux_board_nucleo_l476rg.o
obj-$(CONFIG_BOARD_OLIMEXINO_STM32) += stm32/pinmux_board_olimexino_stm32.o
obj-$(CONFIG_BOARD_STM32_MINI_A15) += stm32/pinmux_board_stm32_mini_a15.o

# "runtime" pinmux
obj-$(CONFIG_PINMUX_DEV) += dev/
