#----------------------------------------------------------------------------------------------
#  Makefile
#----------------------------------------------------------------------------------------------

include config.mak

vpath %.c $(SRCDIR)
vpath %.h $(SRCDIR)

OBJ_MP4OPUSENC = $(SRC_MP4OPUSENC:%.c=%.o)
OBJ_MP4OPUSDEC = $(SRC_MP4OPUSDEC:%.c=%.o)

SRC_ALL = $(SRC_MP4OPUSENC) $(SRC_MP4OPUSDEC)

ifneq ($(STRIP),)
LDFLAGS += -Wl,-s
endif

.PHONY: all clean distclean dep

all: $(MP4OPUSENC) $(MP4OPUSDEC)

$(MP4OPUSENC): $(OBJ_MP4OPUSENC)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

$(MP4OPUSDEC): $(OBJ_MP4OPUSDEC)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c .depend
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) *.exe *.o .depend

distclean: clean
	$(RM) config.*

dep: .depend

ifneq ($(wildcard .depend),)
include .depend
endif

.depend: config.mak
	@$(RM) .depend
	@$(foreach SRC, $(SRC_ALL:%=$(SRCDIR)/%), $(CC) $(SRC) $(CFLAGS) -g0 -MT $(SRC:$(SRCDIR)/%.c=%.o) -MM >> .depend;)

config.mak:
	configure
