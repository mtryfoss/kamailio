#
# db_nats module
#
# WARNING: do not run this directly, it should be run by the main Makefile

include ../../Makefile.defs
auto_gen=
NAME=db_nats.so

ifeq ($(CROSS_COMPILE),)
NATS_BUILDER=$(shell \
	if pkg-config --exists libnats; then \
		echo 'pkg-config libnats'; \
	fi)
endif

ifneq ($(NATS_BUILDER),)
	DEFS += $(shell $(NATS_BUILDER) --cflags)
	LIBS += $(shell $(NATS_BUILDER) --libs)
else
	DEFS += -I$(LOCALBASE)/include
	LIBS += -L$(LOCALBASE)/lib -lnats
endif

include ../../Makefile.modules
