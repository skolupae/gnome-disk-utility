
NULL =
BUILT_SOURCES =

bin_PROGRAMS = gnome-disks

gduenumtypes.h: gduenums.h gduenumtypes.h.template
	( top_builddir=`cd $(top_builddir) && pwd`; 						\
	 cd $(srcdir) && glib-mkenums --template gduenumtypes.h.template gduenums.h ) > 	\
	   gduenumtypes.h.tmp && mv gduenumtypes.h.tmp gduenumtypes.h				\
	$(NULL)

gduenumtypes.c: gduenums.h gduenumtypes.c.template
	( top_builddir=`cd $(top_builddir) && pwd`; 						\
	 cd $(srcdir) && glib-mkenums --template gduenumtypes.c.template gduenums.h ) > 	\
	   gduenumtypes.c.tmp && mv gduenumtypes.c.tmp gduenumtypes.c				\
	$(NULL)

enum_built_sources =							\
	gduenumtypes.h			gduenumtypes.c			\
	$(NULL)

BUILT_SOURCES += $(enum_built_sources)

gnome_disks_SOURCES = 							\
					main.c				\
	gdu.h								\
	gduapplication.h		gduapplication.c		\
	gdudevicetreemodel.h		gdudevicetreemodel.c		\
	gdutypes.h							\
	gduenums.h							\
	gduutils.h			gduutils.c			\
	gduvolumegrid.h			gduvolumegrid.c			\
	gduwindow.h			gduwindow.c			\
	gduatasmartdialog.h		gduatasmartdialog.c		\
	gducrypttabdialog.h		gducrypttabdialog.c		\
	gdufilesystemdialog.h		gdufilesystemdialog.c		\
	gdufstabdialog.h		gdufstabdialog.c		\
	gdupartitiondialog.h		gdupartitiondialog.c		\
	gduunlockdialog.h		gduunlockdialog.c		\
	gduformatvolumedialog.h		gduformatvolumedialog.c		\
	gducreatepartitiondialog.h	gducreatepartitiondialog.c	\
	gducreatefilesystemwidget.h	gducreatefilesystemwidget.c	\
	gduformatdiskdialog.h		gduformatdiskdialog.c		\
	gducreatediskimagedialog.h	gducreatediskimagedialog.c	\
	gdurestorediskimagedialog.h	gdurestorediskimagedialog.c	\
	gdupasswordstrengthwidget.h	gdupasswordstrengthwidget.c	\
	gduestimator.h			gduestimator.c			\
	gduchangepassphrasedialog.h	gduchangepassphrasedialog.c	\
	$(enum_built_sources)						\
	$(NULL)

gnome_disks_CPPFLAGS = 					\
	-I$(top_srcdir)/src/				\
	-I$(top_builddir)/src/				\
	-DPACKAGE_DATA_DIR=\""$(datadir)"\"		\
	-DG_LOG_DOMAIN=\"GNOME-Disks\"			\
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\"	\
	$(NULL)

gnome_disks_CFLAGS = 					\
	$(GLIB2_CFLAGS)					\
	$(UDISKS2_CFLAGS)				\
	$(GTK3_CFLAGS)					\
	$(KEYRING1_CFLAGS)				\
	$(LIBSYSTEMD_LOGIN_CFLAGS)			\
	$(WARN_CFLAGS)					\
	-lm						\
	$(NULL)

gnome_disks_LDADD = 					\
	$(GLIB2_LIBS)					\
	$(UDISKS2_LIBS)					\
	$(GTK3_LIBS)					\
	$(KEYRING1_LIBS)				\
	$(LIBSYSTEMD_LOGIN_LIBS)			\
	$(NULL)

EXTRA_DIST = 						\
	gduenumtypes.h.template				\
	gduenumtypes.c.template				\
	$(NULL)

clean-local :
	rm -f *~