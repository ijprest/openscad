# Environment variables which can be set to specify library locations:
# MPIRDIR
# MPFRDIR
# BOOSTDIR
# CGALDIR
# EIGENDIR
# GLEWDIR
# OPENCSGDIR
# OPENSCAD_LIBRARIES
#
# Please see the 'Building' sections of the OpenSCAD user manual
# for updated tips & workarounds.
#
# http://en.wikibooks.org/wiki/OpenSCAD_User_Manual

!experimental {
  message("If you're building a development binary, consider adding CONFIG+=experimental")
}

isEmpty(QT_VERSION) {
  error("Please use qmake for Qt 4 or Qt 5 (probably qmake-qt4)")
}

# Auto-include config_<variant>.pri if the VARIANT variable is give on the
# command-line, e.g. qmake VARIANT=mybuild
!isEmpty(VARIANT) {
  message("Variant: $${VARIANT}")
  exists(config_$${VARIANT}.pri) {
    message("Including config_$${VARIANT}.pri")
    include(config_$${VARIANT}.pri)
  }
}

# Populate VERSION, VERSION_YEAR, VERSION_MONTH, VERSION_DATE from system date
include(version.pri)

debug: DEFINES += DEBUG

TEMPLATE = app

INCLUDEPATH += src
DEPENDPATH += src

# Handle custom library location.
# Used when manually installing 3rd party libraries
isEmpty(OPENSCAD_LIBDIR) OPENSCAD_LIBDIR = $$(OPENSCAD_LIBRARIES)
macx:isEmpty(OPENSCAD_LIBDIR) {
  exists(/opt/local):exists(/usr/local/Cellar) {
    error("It seems you might have libraries in both /opt/local and /usr/local. Please specify which one to use with qmake OPENSCAD_LIBDIR=<prefix>")
  } else {
    exists(/opt/local) {
      #Default to MacPorts on Mac OS X
      message("Automatically searching for libraries in /opt/local. To override, use qmake OPENSCAD_LIBDIR=<prefix>")
      OPENSCAD_LIBDIR = /opt/local
    } else:exists(/usr/local/Cellar) {
      message("Automatically searching for libraries in /usr/local. To override, use qmake OPENSCAD_LIBDIR=<prefix>")
      OPENSCAD_LIBDIR = /usr/local
    }
  }
}
!isEmpty(OPENSCAD_LIBDIR) {
  QMAKE_INCDIR = $$OPENSCAD_LIBDIR/include
  QMAKE_LIBDIR = $$OPENSCAD_LIBDIR/lib
}

# add CONFIG+=deploy to the qmake command-line to make a deployment build
deploy {
  message("Building deployment version")
  DEFINES += OPENSCAD_DEPLOY
  macx: CONFIG += sparkle
}

macx {
  TARGET = OpenSCAD
}
else {
  TARGET = openscad
}

macx {
  ICON = icons/OpenSCAD.icns
  QMAKE_INFO_PLIST = Info.plist
  APP_RESOURCES.path = Contents/Resources
  APP_RESOURCES.files = OpenSCAD.sdef dsa_pub.pem icons/SCAD.icns
  QMAKE_BUNDLE_DATA += APP_RESOURCES
  LIBS += -framework Cocoa -framework ApplicationServices

  # Mac needs special care to link against the correct C++ library
  # We attempt to auto-detect it by inspecting Boost
  dirs = $${BOOSTDIR} $${QMAKE_LIBDIR}
  for(dir, dirs) {
    system(grep -q __112basic_string $${dir}/libboost_thread* >& /dev/null) {
      message("Detected libc++-linked boost in $${dir}")
      CONFIG += libc++
    }
  }

  libc++ {
    QMAKE_CXXFLAGS += -stdlib=libc++
    QMAKE_LFLAGS += -stdlib=libc++
    QMAKE_OBJECTIVE_CFLAGS += -stdlib=libc++
    # libc++ on requires Mac OS X 10.7+
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.7
  }
}

win* {
  RC_FILE = openscad_win32.rc
  QTPLUGIN += qtaccessiblewidgets
}

CONFIG += qt
QT += opengl

# see http://fedoraproject.org/wiki/UnderstandingDSOLinkChange
# and https://github.com/openscad/openscad/pull/119
# ( QT += opengl does not automatically link glu on some DSO systems. )
unix:!macx {
  QMAKE_LIBS_OPENGL *= -lGLU
  QMAKE_LIBS_OPENGL *= -lX11
}

netbsd* {
   QMAKE_LFLAGS += -L/usr/X11R7/lib
   QMAKE_LFLAGS += -Wl,-R/usr/X11R7/lib
   QMAKE_LFLAGS += -Wl,-R/usr/pkg/lib
   !clang: { QMAKE_CXXFLAGS += -std=c++0x }
   # FIXME: Can the lines below be removed in favour of the OPENSCAD_LIBDIR handling above?
   !isEmpty(OPENSCAD_LIBDIR) {
     QMAKE_CFLAGS = -I$$OPENSCAD_LIBDIR/include $$QMAKE_CFLAGS
     QMAKE_CXXFLAGS = -I$$OPENSCAD_LIBDIR/include $$QMAKE_CXXFLAGS
     QMAKE_LFLAGS = -L$$OPENSCAD_LIBDIR/lib $$QMAKE_LFLAGS
     QMAKE_LFLAGS = -Wl,-R$$OPENSCAD_LIBDIR/lib $$QMAKE_LFLAGS
   }
}

# Prevent LD_LIBRARY_PATH problems when running the openscad binary
# on systems where uni-build-dependencies.sh was used.
# Will not affect 'normal' builds.
!isEmpty(OPENSCAD_LIBDIR) {
  unix:!macx {
    QMAKE_LFLAGS = -Wl,-R$$OPENSCAD_LIBDIR/lib $$QMAKE_LFLAGS
    # need /lib64 beause GLEW installs itself there on 64 bit machines
    QMAKE_LFLAGS = -Wl,-R$$OPENSCAD_LIBDIR/lib64 $$QMAKE_LFLAGS
  }
}

# See Dec 2011 OpenSCAD mailing list, re: CGAL/GCC bugs.
*g++* {
  QMAKE_CXXFLAGS *= -fno-strict-aliasing
  QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-local-typedefs # ignored before 4.8
}

*clang* {
  # http://llvm.org/bugs/show_bug.cgi?id=9182
  QMAKE_CXXFLAGS_WARN_ON += -Wno-overloaded-virtual
  # disable enormous amount of warnings about CGAL / boost / etc
  QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-parameter
  QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-variable
  QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-function
  QMAKE_CXXFLAGS_WARN_ON += -Wno-c++11-extensions
  # might want to actually turn this on once in a while
  QMAKE_CXXFLAGS_WARN_ON += -Wno-sign-compare
}

CONFIG(skip-version-check) {
  # force the use of outdated libraries
  DEFINES += OPENSCAD_SKIP_VERSION_CHECK
}

# Application configuration
macx:CONFIG += mdi
CONFIG += cgal
CONFIG += opencsg
CONFIG += boost
CONFIG += eigen
CONFIG += glib-2.0
CONFIG += harfbuzz
CONFIG += freetype
CONFIG += fontconfig

#Uncomment the following line to enable the QScintilla editor
CONFIG += scintilla

# Make experimental features available
experimental {
  DEFINES += ENABLE_EXPERIMENTAL
}

mdi {
  DEFINES += ENABLE_MDI
}

include(common.pri)

# mingw has to come after other items so OBJECT_DIRS will work properly
CONFIG(mingw-cross-env) {
  include(mingw-cross-env.pri)
}

win* {
  FLEXSOURCES = src/lexer.l
  BISONSOURCES = src/parser.y
} else {
  LEXSOURCES += src/lexer.l
  YACCSOURCES += src/parser.y
}

RESOURCES = openscad.qrc

FORMS += src/MainWindow.ui \
           src/Preferences.ui \
           src/OpenCSGWarningDialog.ui \
           src/AboutDialog.ui \
           src/FontListDialog.ui \
           src/ProgressWidget.ui \
           src/launchingscreen.ui \
           src/LibraryInfoDialog.ui

HEADERS += src/typedefs.h \
           src/version_check.h \
           src/ProgressWidget.h \
           src/parsersettings.h \
           src/renderer.h \
           src/rendersettings.h \
           src/colormap.h \
           src/ThrownTogetherRenderer.h \
           src/CGAL_OGL_Polyhedron.h \
           src/OGL_helper.h \
           src/QGLView.h \
           src/GLView.h \
           src/MainWindow.h \
           src/Preferences.h \
           src/OpenCSGWarningDialog.h \
           src/AboutDialog.h \
           src/FontListDialog.h \
           src/builtin.h \
           src/calc.h \
           src/context.h \
           src/modcontext.h \
           src/evalcontext.h \
           src/csgterm.h \
           src/csgtermnormalizer.h \
           src/dxfdata.h \
           src/dxfdim.h \
           src/export.h \
           src/expression.h \
           src/function.h \
           src/grid.h \
           src/highlighter.h \
           src/localscope.h \
           src/module.h \
           src/feature.h \
           src/node.h \
           src/csgnode.h \
           src/offsetnode.h \
           src/linearextrudenode.h \
           src/rotateextrudenode.h \
           src/projectionnode.h \
           src/cgaladvnode.h \
           src/importnode.h \
           src/transformnode.h \
           src/colornode.h \
           src/rendernode.h \
           src/textnode.h \
           src/openscad.h \
           src/handle_dep.h \
           src/Geometry.h \
           src/Polygon2d.h \
           src/clipper-utils.h \
           src/polyset-utils.h \
           src/polyset.h \
           src/printutils.h \
           src/fileutils.h \
           src/value.h \
           src/progress.h \
           src/editor.h \
           src/visitor.h \
           src/state.h \
           src/traverser.h \
           src/nodecache.h \
           src/nodedumper.h \
           src/ModuleCache.h \
           src/GeometryCache.h \
           src/GeometryEvaluator.h \
           src/CSGTermEvaluator.h \
           src/Tree.h \
src/DrawingCallback.h \
src/FreetypeRenderer.h \
src/FontCache.h \
           src/mathc99.h \
           src/memory.h \
           src/linalg.h \
           src/Camera.h \
           src/system-gl.h \
           src/stl-utils.h \
           src/boost-utils.h \
           src/LibraryInfo.h \
           src/svg.h \
           \
           src/lodepng.h \
           src/OffscreenView.h \
           src/OffscreenContext.h \
           src/OffscreenContextAll.hpp \
           src/fbo.h \
           src/imageutils.h \
           src/system-gl.h \
           src/CsgInfo.h \
           \
           src/AutoUpdater.h \
           src/launchingscreen.h \
           src/legacyeditor.h \
           src/LibraryInfoDialog.h

SOURCES += src/version_check.cc \
           src/ProgressWidget.cc \
           src/mathc99.cc \
           src/linalg.cc \
           src/Camera.cc \
           src/handle_dep.cc \
           src/value.cc \
           src/expr.cc \
           src/func.cc \
           src/localscope.cc \
           src/module.cc \
           src/feature.cc \
           src/node.cc \
           src/context.cc \
           src/modcontext.cc \
           src/evalcontext.cc \
           src/csgterm.cc \
           src/csgtermnormalizer.cc \
           src/Geometry.cc \
           src/Polygon2d.cc \
           src/clipper-utils.cc \
           src/polyset-utils.cc \
           src/polyset.cc \
           src/csgops.cc \
           src/transform.cc \
           src/color.cc \
           src/primitives.cc \
           src/projection.cc \
           src/cgaladv.cc \
           src/surface.cc \
           src/control.cc \
           src/render.cc \
           src/text.cc \
           src/dxfdata.cc \
           src/dxfdim.cc \
           src/offset.cc \
           src/linearextrude.cc \
           src/rotateextrude.cc \
           src/printutils.cc \
           src/fileutils.cc \
           src/progress.cc \
           src/parsersettings.cc \
           src/stl-utils.cc \
           src/boost-utils.cc \
           src/PlatformUtils.cc \
           src/LibraryInfo.cc \
           \
           src/nodedumper.cc \
           src/traverser.cc \
           src/GeometryEvaluator.cc \
           src/ModuleCache.cc \
           src/GeometryCache.cc \
           src/Tree.cc \
src/DrawingCallback.cc \
src/FreetypeRenderer.cc \
src/FontCache.cc \
           \
           src/rendersettings.cc \
           src/highlighter.cc \
           src/Preferences.cc \
           src/OpenCSGWarningDialog.cc \
           src/editor.cc \
           src/GLView.cc \
           src/QGLView.cc \
           src/AutoUpdater.cc \
           \
           src/builtin.cc \
           src/calc.cc \
           src/export.cc \
           src/export_png.cc \
           src/import.cc \
           src/renderer.cc \
           src/colormap.cc \
           src/ThrownTogetherRenderer.cc \
           src/CSGTermEvaluator.cc \
           src/svg.cc \
           src/OffscreenView.cc \
           src/fbo.cc \
           src/system-gl.cc \
           src/imageutils.cc \
           src/lodepng.cpp \
           \
           src/openscad.cc \
           src/mainwin.cc \
           src/UIUtils.cc \
	   src/FontListDialog.cc \
           src/launchingscreen.cpp \
           src/legacyeditor.cc \
           src/LibraryInfoDialog.cc

# ClipperLib
SOURCES += src/polyclipping/clipper.cpp
HEADERS += src/polyclipping/clipper.hpp

unix:!macx {
  SOURCES += src/imageutils-lodepng.cc
  SOURCES += src/OffscreenContextGLX.cc
}
macx {
  SOURCES += src/imageutils-macosx.cc
  OBJECTIVE_SOURCES += src/OffscreenContextCGL.mm
}
win* {
  SOURCES += src/imageutils-lodepng.cc
  SOURCES += src/OffscreenContextWGL.cc
}

opencsg {
  HEADERS += src/OpenCSGRenderer.h
  SOURCES += src/OpenCSGRenderer.cc
}

cgal {
HEADERS += src/cgal.h \
           src/cgalfwd.h \
           src/cgalutils.h \
           src/Reindexer.h \
           src/CGALCache.h \
           src/CGALRenderer.h \
           src/CGAL_Nef_polyhedron.h \
           src/CGAL_Nef3_workaround.h \
           src/cgalworker.h \
           src/Polygon2d-CGAL.h

SOURCES += src/cgalutils.cc \
           src/cgalutils-tess.cc \
           src/CGALCache.cc \
           src/CGALRenderer.cc \
           src/CGAL_Nef_polyhedron.cc \
           src/CGAL_Nef_polyhedron_DxfData.cc \
           src/cgalworker.cc \
           src/Polygon2d-CGAL.cc
}

macx {
  HEADERS += src/AppleEvents.h \
             src/EventFilter.h \
             src/CocoaUtils.h
  SOURCES += src/AppleEvents.cc
  OBJECTIVE_SOURCES += src/CocoaUtils.mm \
                       src/PlatformUtils-mac.mm
}
unix:!macx {
  SOURCES += src/PlatformUtils-posix.cc
}
win* {
  SOURCES += src/PlatformUtils-win.cc
}

isEmpty(PREFIX):PREFIX = /usr/local

target.path = $$PREFIX/bin/
INSTALLS += target

examples.path = $$PREFIX/share/openscad/examples/
examples.files = examples/*
INSTALLS += examples

libraries.path = $$PREFIX/share/openscad/libraries/
libraries.files = libraries/*
INSTALLS += libraries

applications.path = $$PREFIX/share/applications
applications.files = icons/openscad.desktop
INSTALLS += applications

mimexml.path = $$PREFIX/share/mime/packages
mimexml.files = icons/openscad.xml
INSTALLS += mimexml

appdata.path = $$PREFIX/share/appdata
appdata.files = openscad.appdata.xml
INSTALLS += appdata

icons.path = $$PREFIX/share/pixmaps
icons.files = icons/openscad.png
INSTALLS += icons

man.path = $$PREFIX/share/man/man1
man.files = doc/openscad.1
INSTALLS += man

CONFIG(winconsole) {
  include(winconsole.pri)
}
