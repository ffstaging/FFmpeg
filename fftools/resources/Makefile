clean::
	$(RM) $(CLEANSUFFIXES:%=fftools/resources/%)


HTML_RESOURCES := $(SRC_PATH)/fftools/resources/graph.html \

# .html => (gzip) .html.gz => (bin2c) .html.c => (cc) .o
HTML_RESOURCES_GZ := $(HTML_RESOURCES:.html=.html.gz)
HTML_RESOURCES_C := $(HTML_RESOURCES_GZ:.html.gz=.html.c)
HTML_RESOURCES_OBJS := $(HTML_RESOURCES_C:.c=.o)

CSS_RESOURCES := $(SRC_PATH)/fftools/resources/graph.css   \

# .css => (sh) .css.min  => (gzip) .css.min.gz => (bin2c) .css.c => (cc) .o
CSS_RESOURCES_MIN := $(CSS_RESOURCES:.css=.css.min)
CSS_RESOURCES_GZ := $(CSS_RESOURCES_MIN:.css.min=.css.min.gz)
CSS_RESOURCES_C := $(CSS_RESOURCES_GZ:.css.min.gz=.css.c)
CSS_RESOURCES_OBJS := $(CSS_RESOURCES_C:.c=.o)

# Uncomment to prevent deletion
#.PRECIOUS: %.css.c %.css.min %.css.gz %.css.min.gz %.html.gz %.html.c

OBJS-resman +=                  \
    fftools/resources/resman.o          \
    $(HTML_RESOURCES_OBJS)      \
    $(CSS_RESOURCES_OBJS)       \

