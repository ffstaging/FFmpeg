AVPROGS-$(CONFIG_FFMPEG)   += ffmpeg
AVPROGS-$(CONFIG_FFPLAY)   += ffplay
AVPROGS-$(CONFIG_FFPROBE)  += ffprobe

AVPROGS     := $(AVPROGS-yes:%=%$(PROGSSUF)$(EXESUF))
PROGS       += $(AVPROGS)

FFMPEG_SG_SCRIPT = $(SRC_PATH)/tools/ffmpeg-sg$(if $(filter .exe,$(EXESUF)),.cmd)
FFMPEG_SG_TARGET = ffmpeg-sg$(EXESUF)
FFMPEG_SG_ENABLED = $(and $(filter $(CONFIG_FFMPEG),yes),$(or $(filter $(target_os),win32),$(if $(target_os),,yes)))
FFMPEG_SG_FILES = $(if $(FFMPEG_SG_ENABLED),$(FFMPEG_SG_TARGET))

AVBASENAMES  = ffmpeg ffplay ffprobe
ALLAVPROGS   = $(AVBASENAMES:%=%$(PROGSSUF)$(EXESUF))
ALLAVPROGS_G = $(AVBASENAMES:%=%$(PROGSSUF)_g$(EXESUF))

include $(SRC_PATH)/fftools/resources/Makefile

OBJS-ffmpeg +=                  \
    fftools/ffmpeg_dec.o        \
    fftools/ffmpeg_demux.o      \
    fftools/ffmpeg_enc.o        \
    fftools/ffmpeg_filter.o     \
    fftools/ffmpeg_hw.o         \
    fftools/ffmpeg_mux.o        \
    fftools/ffmpeg_mux_init.o   \
    fftools/ffmpeg_opt.o        \
    fftools/ffmpeg_sched.o      \
    fftools/graph/graphprint.o        \
    fftools/sync_queue.o        \
    fftools/thread_queue.o      \
    fftools/textformat/avtextformat.o \
    fftools/textformat/tf_compact.o   \
    fftools/textformat/tf_default.o   \
    fftools/textformat/tf_flat.o      \
    fftools/textformat/tf_ini.o       \
    fftools/textformat/tf_json.o      \
    fftools/textformat/tf_mermaid.o   \
    fftools/textformat/tf_xml.o       \
    fftools/textformat/tw_avio.o      \
    fftools/textformat/tw_buffer.o    \
    fftools/textformat/tw_stdout.o    \
    $(OBJS-resman)                    \
    $(RESOBJS)                        \

OBJS-ffprobe +=                       \
    fftools/textformat/avtextformat.o \
    fftools/textformat/tf_compact.o   \
    fftools/textformat/tf_default.o   \
    fftools/textformat/tf_flat.o      \
    fftools/textformat/tf_ini.o       \
    fftools/textformat/tf_json.o      \
    fftools/textformat/tf_mermaid.o   \
    fftools/textformat/tf_xml.o       \
    fftools/textformat/tw_avio.o      \
    fftools/textformat/tw_buffer.o    \
    fftools/textformat/tw_stdout.o    \

OBJS-ffmpeg += $(COMPAT_OBJS:%=compat/%)
OBJS-ffplay += fftools/ffplay_renderer.o

define DOFFTOOL
OBJS-$(1) += fftools/cmdutils.o fftools/opt_common.o fftools/$(1).o $(OBJS-$(1)-yes)
ifdef HAVE_GNU_WINDRES
OBJS-$(1) += fftools/fftoolsres.o
endif
$(1)$(PROGSSUF)_g$(EXESUF): $$(OBJS-$(1))
$$(OBJS-$(1)): | fftools fftools/textformat fftools/resources fftools/graph
$$(OBJS-$(1)): CFLAGS  += $(CFLAGS-$(1))
$(1)$(PROGSSUF)_g$(EXESUF): LDFLAGS += $(LDFLAGS-$(1))
$(1)$(PROGSSUF)_g$(EXESUF): FF_EXTRALIBS += $(EXTRALIBS-$(1))
-include $$(OBJS-$(1):.o=.d)
endef

$(foreach P,$(AVPROGS-yes),$(eval $(call DOFFTOOL,$(P))))

all: $(AVPROGS) $(FFMPEG_SG_FILES)

$(FFMPEG_SG_TARGET): $(FFMPEG_SG_SCRIPT)
	$(Q)$(CP) -p $(FFMPEG_SG_SCRIPT) $(FFMPEG_SG_TARGET)

fftools/ffprobe.o fftools/cmdutils.o: libavutil/ffversion.h | fftools
OUTDIRS += fftools
OUTDIRS += fftools/textformat
OUTDIRS += fftools/resources
OUTDIRS += fftools/graph

ifdef AVPROGS
install: install-progs install-data
endif

install-progs-yes:
install-progs-$(CONFIG_SHARED): install-libs

install-progs: install-progs-yes $(AVPROGS)
	$(Q)mkdir -p "$(BINDIR)"
	$(INSTALL) -c -m 755 $(AVPROGS) "$(BINDIR)"
	$(if $(FFMPEG_SG_FILES),$(INSTALL) -c -m 755 $(FFMPEG_SG_FILES) "$(BINDIR)")

uninstall: uninstall-progs

uninstall-progs:
	$(RM) $(addprefix "$(BINDIR)/", $(ALLAVPROGS))
	$(if $(FFMPEG_SG_FILES),$(RM) "$(BINDIR)/$(FFMPEG_SG_FILES)")

clean::
	$(RM) $(ALLAVPROGS) $(ALLAVPROGS_G) $(CLEANSUFFIXES:%=fftools/%) $(CLEANSUFFIXES:%=fftools/graph/%) $(CLEANSUFFIXES:%=fftools/textformat/%) $(FFMPEG_SG_FILES)
