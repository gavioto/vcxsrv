TTYAPP = bdftopcf

INCLUDELIBFILES = \
 $(MHMAKECONF)\zlib\$(OBJDIR)\zlib1.lib \
 $(MHMAKECONF)\libXfont\src\fontfile\$(OBJDIR)\libfontfile.lib \
 $(MHMAKECONF)\libXfont\src\bitmap\$(OBJDIR)\libbitmap.lib \
 $(MHMAKECONF)\libXfont\src\util\$(OBJDIR)\libutil.lib

LIBDIRS=$(dir $(INCLUDELIBFILES))

load_makefile $(LIBDIRS:%$(OBJDIR)\=%makefile MAKESERVER=0 DEBUG=$(DEBUG);)

CSRCS = bdftopcf.c

LINKLIBS += $(FREETYPELIB)

