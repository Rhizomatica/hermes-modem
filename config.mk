# config.mk - Shared build configuration for HERMES Modem
#
# All Makefiles include this file to get consistent CC, AR, and base CFLAGS.
# Each subdirectory appends its own local include paths and extra flags.
#
# Override on the command line for cross-compilation, e.g.:
#   make CC=arm-linux-gnueabihf-gcc
#   make CC=clang COMMON_CFLAGS="-Wall -O2 -std=gnu11 -march=armv7-a"

ifeq ($(origin CC),default)
CC = gcc
endif

ifeq ($(origin AR),default)
AR = ar
endif

COMMON_CFLAGS ?= -Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE
