/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include "GeometryCache.h"
#include "ModuleCache.h"
#include "MainWindow.h"
#include "parsersettings.h"
#include "rendersettings.h"
#include "Preferences.h"
#include "printutils.h"
#include "node.h"
#include "polyset.h"
#include "csgterm.h"
#include "highlighter.h"
#include "export.h"
#include "builtin.h"
#include "expression.h"
#include "progress.h"
#include "dxfdim.h"
#include "legacyeditor.h"
#ifdef USE_SCINTILLA_EDITOR
#include "scintillaeditor.h"
#endif
#include "AboutDialog.h"
#include "FontListDialog.h"
#include "LibraryInfoDialog.h"
#ifdef ENABLE_OPENCSG
#include "CSGTermEvaluator.h"
#include "OpenCSGRenderer.h"
#include <opencsg.h>
#endif
#include "ProgressWidget.h"
#include "ThrownTogetherRenderer.h"
#include "csgtermnormalizer.h"
#include "QGLView.h"
#include "AutoUpdater.h"
#ifdef Q_OS_MAC
#include "CocoaUtils.h"
#endif
#include "PlatformUtils.h"

#include <QMenu>
#include <QTime>
#include <QMenuBar>
#include <QSplitter>
#include <QFileDialog>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QFileInfo>
#include <QTextStream>
#include <QStatusBar>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QTimer>
#include <QMessageBox>
#include <QDesktopServices>
#include <QSettings>
#include <QProgressDialog>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <QDockWidget>

#include <fstream>

#include <algorithm>
#include <boost/version.hpp>
#include <boost/foreach.hpp>
#include <sys/stat.h>

#ifdef ENABLE_CGAL

#include "CGALCache.h"
#include "GeometryEvaluator.h"
#include "CGALRenderer.h"
#include "CGAL_Nef_polyhedron.h"
#include "cgal.h"
#include "cgalworker.h"
#include "cgalutils.h"

#endif // ENABLE_CGAL

#include "boosty.h"
#include "FontCache.h"

// Keeps track of open window
QSet<MainWindow*> *MainWindow::windows = NULL;

// Global application state
unsigned int GuiLocker::gui_locked = 0;

#define QUOTE(x__) # x__
#define QUOTED(x__) QUOTE(x__)

static char helptitle[] =
	"OpenSCAD " QUOTED(OPENSCAD_VERSION)
#ifdef OPENSCAD_COMMIT
	" (git " QUOTED(OPENSCAD_COMMIT) ")"
#endif
	"\nhttp://www.openscad.org\n\n";
static char copyrighttext[] =
	"Copyright (C) 2009-2014 The OpenSCAD Developers\n"
	"\n"
	"This program is free software; you can redistribute it and/or modify "
	"it under the terms of the GNU General Public License as published by "
	"the Free Software Foundation; either version 2 of the License, or "
	"(at your option) any later version.";

static void
settings_setValueList(const QString &key,const QList<int> &list)
{
	QSettings settings;
	settings.beginWriteArray(key);
	for (int i=0;i<list.size(); ++i) {
		settings.setArrayIndex(i);
		settings.setValue("entry",list[i]);
	}
	settings.endArray();
}

QList<int>
settings_valueList(const QString &key, const QList<int> &defaultList = QList<int>())
{
	QSettings settings;
	QList<int> result;
	if (settings.contains(key+"/size")){
		int length = settings.beginReadArray(key);
		for (int i = 0; i < length; ++i) {
			settings.setArrayIndex(i);
			result += settings.value("entry").toInt();
		}
		settings.endArray();
		return result;
	} else {
		return defaultList;
	}

}

bool MainWindow::mdiMode = false;
bool MainWindow::undockMode = false;

MainWindow::MainWindow(const QString &filename)
    : root_inst("group"), library_info_dialog(NULL), font_list_dialog(NULL), tempFile(NULL), progresswidget(NULL)
{
	setupUi(this);

 	editortype = Preferences::inst()->getValue("editor/editortype").toString();

	useScintilla = (editortype == "QScintilla Editor");
#ifdef USE_SCINTILLA_EDITOR
	if (useScintilla) {
		 editor = new ScintillaEditor(editorDockContents);
	}
	else
#endif
	    editor = new LegacyEditor(editorDockContents);

	editorDockContents->layout()->addWidget(editor);

	setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
	setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
	setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
	setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

	this->setAttribute(Qt::WA_DeleteOnClose);

	if (!MainWindow::windows) MainWindow::windows = new QSet<MainWindow*>;
	MainWindow::windows->insert(this);

#ifdef ENABLE_CGAL
	this->cgalworker = new CGALWorker();
	connect(this->cgalworker, SIGNAL(done(shared_ptr<const Geometry>)), 
					this, SLOT(actionRenderDone(shared_ptr<const Geometry>)));
#endif

	top_ctx.registerBuiltin();

	root_module = NULL;
	absolute_root_node = NULL;
	this->root_chain = NULL;
#ifdef ENABLE_CGAL
	this->cgalRenderer = NULL;
#endif
#ifdef ENABLE_OPENCSG
	this->opencsgRenderer = NULL;
#endif
	this->thrownTogetherRenderer = NULL;

	highlights_chain = NULL;
	background_chain = NULL;
	root_node = NULL;

	tval = 0;
	fps = 0;
	fsteps = 1;
	isClosing = false;

	const QString importStatement = "import(\"%1\");\n";
	const QString surfaceStatement = "surface(\"%1\");\n";
	knownFileExtensions["stl"] = importStatement;
	knownFileExtensions["off"] = importStatement;
	knownFileExtensions["dxf"] = importStatement;
	knownFileExtensions["dat"] = surfaceStatement;
	knownFileExtensions["png"] = surfaceStatement;
	knownFileExtensions["scad"] = "";
	knownFileExtensions["csg"] = "";
	
	editActionZoomIn->setShortcuts(QList<QKeySequence>() << editActionZoomIn->shortcuts() << QKeySequence("CTRL+="));

	connect(this, SIGNAL(highlightError(int)), editor, SLOT(highlightError(int)));
	connect(this, SIGNAL(unhighlightLastError()), editor, SLOT(unhighlightLastError()));

	this->qglview->statusLabel = new QLabel(this);
	statusBar()->addWidget(this->qglview->statusLabel);

	animate_timer = new QTimer(this);
	connect(animate_timer, SIGNAL(timeout()), this, SLOT(updateTVal()));

	autoReloadTimer = new QTimer(this);
	autoReloadTimer->setSingleShot(false);
	autoReloadTimer->setInterval(200);
	connect(autoReloadTimer, SIGNAL(timeout()), this, SLOT(checkAutoReload()));

	waitAfterReloadTimer = new QTimer(this);
	waitAfterReloadTimer->setSingleShot(true);
	waitAfterReloadTimer->setInterval(200);
	connect(waitAfterReloadTimer, SIGNAL(timeout()), this, SLOT(waitAfterReload()));

	connect(this->e_tval, SIGNAL(textChanged(QString)), this, SLOT(actionRenderPreview()));
	connect(this->e_fps, SIGNAL(textChanged(QString)), this, SLOT(updatedFps()));

	animate_panel->hide();
	find_panel->hide();

	// Application menu
#ifdef DEBUG
	this->appActionUpdateCheck->setEnabled(false);
#else
#ifdef Q_OS_MAC
	this->appActionUpdateCheck->setMenuRole(QAction::ApplicationSpecificRole);
	this->appActionUpdateCheck->setEnabled(true);
	connect(this->appActionUpdateCheck, SIGNAL(triggered()), this, SLOT(actionUpdateCheck()));
#endif
#endif
	// File menu
	connect(this->fileActionNew, SIGNAL(triggered()), this, SLOT(actionNew()));
	connect(this->fileActionOpen, SIGNAL(triggered()), this, SLOT(actionOpen()));
	connect(this->fileActionSave, SIGNAL(triggered()), this, SLOT(actionSave()));
	connect(this->fileActionSaveAs, SIGNAL(triggered()), this, SLOT(actionSaveAs()));
	connect(this->fileActionReload, SIGNAL(triggered()), this, SLOT(actionReload()));
	connect(this->fileActionQuit, SIGNAL(triggered()), this, SLOT(quit()));
	connect(this->fileShowLibraryFolder, SIGNAL(triggered()), this, SLOT(actionShowLibraryFolder()));
#ifndef __APPLE__
	QList<QKeySequence> shortcuts = this->fileActionSave->shortcuts();
	shortcuts.push_back(QKeySequence(Qt::Key_F2));
	this->fileActionSave->setShortcuts(shortcuts);
	shortcuts = this->fileActionReload->shortcuts();
	shortcuts.push_back(QKeySequence(Qt::Key_F3));
	this->fileActionReload->setShortcuts(shortcuts);
#endif
	// Open Recent
	for (int i = 0;i<UIUtils::maxRecentFiles; i++) {
		this->actionRecentFile[i] = new QAction(this);
		this->actionRecentFile[i]->setVisible(false);
		this->menuOpenRecent->addAction(this->actionRecentFile[i]);
		connect(this->actionRecentFile[i], SIGNAL(triggered()),
						this, SLOT(actionOpenRecent()));
	}
	this->menuOpenRecent->addSeparator();
	this->menuOpenRecent->addAction(this->fileActionClearRecent);
	connect(this->fileActionClearRecent, SIGNAL(triggered()),
					this, SLOT(clearRecentFiles()));

	show_examples();

	// Edit menu
	connect(this->editActionUndo, SIGNAL(triggered()), editor, SLOT(undo()));
	connect(this->editActionRedo, SIGNAL(triggered()), editor, SLOT(redo()));
	connect(this->editActionCut, SIGNAL(triggered()), editor, SLOT(cut()));
	connect(this->editActionCopy, SIGNAL(triggered()), editor, SLOT(copy()));
	connect(this->editActionPaste, SIGNAL(triggered()), editor, SLOT(paste()));
	connect(this->editActionIndent, SIGNAL(triggered()), editor, SLOT(indentSelection()));
	connect(this->editActionUnindent, SIGNAL(triggered()), editor, SLOT(unindentSelection()));
	connect(this->editActionComment, SIGNAL(triggered()), editor, SLOT(commentSelection()));
	connect(this->editActionUncomment, SIGNAL(triggered()), editor, SLOT(uncommentSelection()));
	connect(this->editActionPasteVPT, SIGNAL(triggered()), this, SLOT(pasteViewportTranslation()));
	connect(this->editActionPasteVPR, SIGNAL(triggered()), this, SLOT(pasteViewportRotation()));
	connect(this->editActionZoomIn, SIGNAL(triggered()), editor, SLOT(zoomIn()));
	connect(this->editActionZoomOut, SIGNAL(triggered()), editor, SLOT(zoomOut()));
	connect(this->editActionHide, SIGNAL(triggered()), this, SLOT(hideEditor()));
	connect(this->editActionPreferences, SIGNAL(triggered()), this, SLOT(preferences()));
	// Edit->Find
	connect(this->editActionFind, SIGNAL(triggered()), this, SLOT(find()));
	connect(this->editActionFindAndReplace, SIGNAL(triggered()), this, SLOT(findAndReplace()));
	connect(this->editActionFindNext, SIGNAL(triggered()), this, SLOT(findNext()));
	connect(this->editActionFindPrevious, SIGNAL(triggered()), this, SLOT(findPrev()));

	// Design menu
	connect(this->designActionAutoReload, SIGNAL(toggled(bool)), this, SLOT(autoReloadSet(bool)));
	connect(this->designActionReloadAndPreview, SIGNAL(triggered()), this, SLOT(actionReloadRenderPreview()));
	connect(this->designActionPreview, SIGNAL(triggered()), this, SLOT(actionRenderPreview()));
#ifdef ENABLE_CGAL
	connect(this->designActionRender, SIGNAL(triggered()), this, SLOT(actionRender()));
#else
	this->designActionRender->setVisible(false);
#endif
	connect(this->designCheckValidity, SIGNAL(triggered()), this, SLOT(actionCheckValidity()));
	connect(this->designActionDisplayAST, SIGNAL(triggered()), this, SLOT(actionDisplayAST()));
	connect(this->designActionDisplayCSGTree, SIGNAL(triggered()), this, SLOT(actionDisplayCSGTree()));
	connect(this->designActionDisplayCSGProducts, SIGNAL(triggered()), this, SLOT(actionDisplayCSGProducts()));
	connect(this->designActionExportSTL, SIGNAL(triggered()), this, SLOT(actionExportSTL()));
	connect(this->designActionExportOFF, SIGNAL(triggered()), this, SLOT(actionExportOFF()));
	connect(this->designActionExportAMF, SIGNAL(triggered()), this, SLOT(actionExportAMF()));
	connect(this->designActionExportDXF, SIGNAL(triggered()), this, SLOT(actionExportDXF()));
	connect(this->designActionExportSVG, SIGNAL(triggered()), this, SLOT(actionExportSVG()));
	connect(this->designActionExportCSG, SIGNAL(triggered()), this, SLOT(actionExportCSG()));
	connect(this->designActionExportImage, SIGNAL(triggered()), this, SLOT(actionExportImage()));
	connect(this->designActionFlushCaches, SIGNAL(triggered()), this, SLOT(actionFlushCaches()));

	// View menu
#ifndef ENABLE_OPENCSG
	this->viewActionPreview->setVisible(false);
#else
	connect(this->viewActionPreview, SIGNAL(triggered()), this, SLOT(viewModePreview()));
	if (!this->qglview->hasOpenCSGSupport()) {
		this->viewActionPreview->setEnabled(false);
	}
#endif

#ifdef ENABLE_CGAL
	connect(this->viewActionSurfaces, SIGNAL(triggered()), this, SLOT(viewModeSurface()));
	connect(this->viewActionWireframe, SIGNAL(triggered()), this, SLOT(viewModeWireframe()));
#else
	this->viewActionSurfaces->setVisible(false);
	this->viewActionWireframe->setVisible(false);
#endif
	connect(this->viewActionThrownTogether, SIGNAL(triggered()), this, SLOT(viewModeThrownTogether()));
	connect(this->viewActionShowEdges, SIGNAL(triggered()), this, SLOT(viewModeShowEdges()));
	connect(this->viewActionShowAxes, SIGNAL(triggered()), this, SLOT(viewModeShowAxes()));
	connect(this->viewActionShowCrosshairs, SIGNAL(triggered()), this, SLOT(viewModeShowCrosshairs()));
	connect(this->viewActionAnimate, SIGNAL(triggered()), this, SLOT(viewModeAnimate()));
	connect(this->viewActionTop, SIGNAL(triggered()), this, SLOT(viewAngleTop()));
	connect(this->viewActionBottom, SIGNAL(triggered()), this, SLOT(viewAngleBottom()));
	connect(this->viewActionLeft, SIGNAL(triggered()), this, SLOT(viewAngleLeft()));
	connect(this->viewActionRight, SIGNAL(triggered()), this, SLOT(viewAngleRight()));
	connect(this->viewActionFront, SIGNAL(triggered()), this, SLOT(viewAngleFront()));
	connect(this->viewActionBack, SIGNAL(triggered()), this, SLOT(viewAngleBack()));
	connect(this->viewActionDiagonal, SIGNAL(triggered()), this, SLOT(viewAngleDiagonal()));
	connect(this->viewActionCenter, SIGNAL(triggered()), this, SLOT(viewCenter()));
	connect(this->viewActionResetView, SIGNAL(triggered()), this, SLOT(viewResetView()));
	connect(this->viewActionViewAll, SIGNAL(triggered()), this, SLOT(viewAll()));
	connect(this->viewActionPerspective, SIGNAL(triggered()), this, SLOT(viewPerspective()));
	connect(this->viewActionOrthogonal, SIGNAL(triggered()), this, SLOT(viewOrthogonal()));
	connect(this->viewActionHide, SIGNAL(triggered()), this, SLOT(hideConsole()));
	connect(this->viewActionZoomIn, SIGNAL(triggered()), qglview, SLOT(ZoomIn()));
	connect(this->viewActionZoomOut, SIGNAL(triggered()), qglview, SLOT(ZoomOut()));

	// Help menu
	connect(this->helpActionAbout, SIGNAL(triggered()), this, SLOT(helpAbout()));
	connect(this->helpActionHomepage, SIGNAL(triggered()), this, SLOT(helpHomepage()));
	connect(this->helpActionManual, SIGNAL(triggered()), this, SLOT(helpManual()));
	connect(this->helpActionLibraryInfo, SIGNAL(triggered()), this, SLOT(helpLibrary()));
	connect(this->helpActionFontInfo, SIGNAL(triggered()), this, SLOT(helpFontInfo()));

	setCurrentOutput();

	PRINT(helptitle);
	PRINT(copyrighttext);
	PRINT("");

	if (!filename.isEmpty()) {
		openFile(filename);
	} else {
		setFileName("");
	}
	updateRecentFileActions();

	connect(editor, SIGNAL(contentsChanged()), this, SLOT(animateUpdateDocChanged()));
	connect(editor, SIGNAL(modificationChanged(bool)), this, SLOT(setWindowModified(bool)));
	connect(this->qglview, SIGNAL(doAnimateUpdate()), this, SLOT(animateUpdate()));

	connect(Preferences::inst(), SIGNAL(requestRedraw()), this->qglview, SLOT(updateGL()));
	connect(Preferences::inst(), SIGNAL(updateMdiMode(bool)), this, SLOT(updateMdiMode(bool)));
	connect(Preferences::inst(), SIGNAL(updateUndockMode(bool)), this, SLOT(updateUndockMode(bool)));
	connect(Preferences::inst(), SIGNAL(fontChanged(const QString&,uint)), 
					editor, SLOT(initFont(const QString&,uint)));
	connect(Preferences::inst(), SIGNAL(openCSGSettingsChanged()),
					this, SLOT(openCSGSettingsChanged()));
	connect(Preferences::inst(), SIGNAL(syntaxHighlightChanged(const QString&)),
					editor, SLOT(setHighlightScheme(const QString&)));
	connect(Preferences::inst(), SIGNAL(colorSchemeChanged(const QString&)), 
					this, SLOT(setColorScheme(const QString&)));
	Preferences::inst()->apply();

	QString cs = Preferences::inst()->getValue("3dview/colorscheme").toString();
	this->setColorScheme(cs);

	//find and replace panel
	connect(this->findTypeComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(selectFindType(int)));
	connect(this->findInputField, SIGNAL(textChanged(QString)), this, SLOT(findString(QString)));
	connect(this->findInputField, SIGNAL(returnPressed()), this->nextButton, SLOT(animateClick()));
	find_panel->installEventFilter(this);

	connect(this->prevButton, SIGNAL(clicked()), this, SLOT(findPrev()));
	connect(this->nextButton, SIGNAL(clicked()), this, SLOT(findNext()));
	connect(this->hideFindButton, SIGNAL(clicked()), find_panel, SLOT(hide()));
	connect(this->replaceButton, SIGNAL(clicked()), this, SLOT(replace()));
	connect(this->replaceAllButton, SIGNAL(clicked()), this, SLOT(replaceAll()));
	connect(this->replaceInputField, SIGNAL(returnPressed()), this->replaceButton, SLOT(animateClick()));

	// make sure it looks nice..
	QSettings settings;
	QByteArray windowState = settings.value("window/state", QByteArray()).toByteArray();
	restoreState(windowState);
	resize(settings.value("window/size", QSize(800, 600)).toSize());
	move(settings.value("window/position", QPoint(0, 0)).toPoint());

	if (windowState.size() == 0) {
		/*
		 * This triggers only in case the configuration file has no
		 * window state information (or no configuration file at all).
		 * When this happens, the editor would default to a very ugly
		 * width due to the dock widget layout. This overwrites the
		 * value reported via sizeHint() to a width a bit smaller than
		 * half the main window size (either the one loaded from the
		 * configuration or the default value of 800).
		 * The height is only a dummy value which will be essentially
		 * ignored by the layouting as the editor is set to expand to
		 * fill the available space.
		 */
		editor->setInitialSizeHint(QSize((5 * this->width() / 11), 100));
	}
	
	connect(this->editorDock, SIGNAL(topLevelChanged(bool)), this, SLOT(editorTopLevelChanged(bool)));
	connect(this->consoleDock, SIGNAL(topLevelChanged(bool)), this, SLOT(consoleTopLevelChanged(bool)));
	
	// display this window and check for OpenGL 2.0 (OpenCSG) support
	viewModeThrownTogether();
	show();

#ifdef ENABLE_OPENCSG
	viewModePreview();
#else
	viewModeThrownTogether();
#endif
	loadViewSettings();
	loadDesignSettings();

	setAcceptDrops(true);
	clearCurrentOutput();
}

void MainWindow::loadViewSettings(){
	QSettings settings;
	if (settings.value("view/showEdges").toBool()) {
		viewActionShowEdges->setChecked(true);
		viewModeShowEdges();
	}
	if (settings.value("view/showAxes").toBool()) {
		viewActionShowAxes->setChecked(true);
		viewModeShowAxes();
	}
	if (settings.value("view/showCrosshairs").toBool()) {
		viewActionShowCrosshairs->setChecked(true);
		viewModeShowCrosshairs();
	}
	if (settings.value("view/orthogonalProjection").toBool()) {
		viewOrthogonal();
	} else {
		viewPerspective();
	}
	viewActionHide->setChecked(settings.value("view/hideConsole").toBool());
	hideConsole();
	editActionHide->setChecked(settings.value("view/hideEditor").toBool());
	hideEditor();
	updateMdiMode(settings.value("advanced/mdi").toBool());
	updateUndockMode(settings.value("advanced/undockableWindows").toBool());
}

void MainWindow::loadDesignSettings()
{
	QSettings settings;
	if (settings.value("design/autoReload").toBool()) {
		designActionAutoReload->setChecked(true);
	}
	uint polySetCacheSize = Preferences::inst()->getValue("advanced/polysetCacheSize").toUInt();
	GeometryCache::instance()->setMaxSize(polySetCacheSize);
#ifdef ENABLE_CGAL
	uint cgalCacheSize = Preferences::inst()->getValue("advanced/cgalCacheSize").toUInt();
	CGALCache::instance()->setMaxSize(cgalCacheSize);
#endif
}

void MainWindow::updateMdiMode(bool mdi)
{
	MainWindow::mdiMode = mdi;
}

void MainWindow::updateUndockMode(bool undockMode)
{
	MainWindow::undockMode = undockMode;
	if (undockMode) {
		editorDock->setFeatures(editorDock->features() | QDockWidget::DockWidgetFloatable);
		consoleDock->setFeatures(consoleDock->features() | QDockWidget::DockWidgetFloatable);
	} else {
		editorDock->setFeatures(editorDock->features() & ~QDockWidget::DockWidgetFloatable);
		consoleDock->setFeatures(consoleDock->features() & ~QDockWidget::DockWidgetFloatable);
	}
}

MainWindow::~MainWindow()
{
	if (root_module) delete root_module;
	if (root_node) delete root_node;
	if (root_chain) delete root_chain;
#ifdef ENABLE_CGAL
	this->root_geom.reset();
	delete this->cgalRenderer;
#endif
#ifdef ENABLE_OPENCSG
	delete this->opencsgRenderer;
#endif
	delete this->thrownTogetherRenderer;
	MainWindow::windows->remove(this);
}

void MainWindow::showProgress()
{
	this->statusBar()->addPermanentWidget(qobject_cast<ProgressWidget*>(sender()));
}

void MainWindow::report_func(const class AbstractNode*, void *vp, int mark)
{
	MainWindow *thisp = static_cast<MainWindow*>(vp);
	int v = (int)((mark*1000.0) / progress_report_count);
	int permille = v < 1000 ? v : 999;
	if (permille > thisp->progresswidget->value()) {
		QMetaObject::invokeMethod(thisp->progresswidget, "setValue", Qt::QueuedConnection,
															Q_ARG(int, permille));
		QApplication::processEvents();
	}

	// FIXME: Check if cancel was requested by e.g. Application quit
	if (thisp->progresswidget->wasCanceled()) throw ProgressCancelException();
}

/*!
	Requests to open a file from an external event, e.g. by double-clicking a filename.
 */
void MainWindow::requestOpenFile(const QString &filename)
{
	// if we have an empty open window, use that one
	QSetIterator<MainWindow *> i(*MainWindow::windows);
	while (i.hasNext()) {
		MainWindow *w = i.next();

		if (w->editor->toPlainText().isEmpty()) {
			w->openFile(filename);
			return;
		}
	}

	// otherwise, create a new one
	new MainWindow(filename);
}

/*!
 	Open the given file. In MDI mode a new window is created if the current
 	one is not empty. Otherwise the current window content is overwritten.
 	Any check whether to replace the content have to be made before.
 */

void MainWindow::openFile(const QString &new_filename)
{
	if (MainWindow::mdiMode) {
		if (!editor->toPlainText().isEmpty()) {
			new MainWindow(new_filename);
			return;
		}
	}

	setCurrentOutput();
	editor->setPlainText("");
	this->last_compiled_doc = "";

	const QFileInfo fileInfo(new_filename);
	const QString suffix = fileInfo.suffix().toLower();
	const bool knownFileType = knownFileExtensions.contains(suffix);
	const QString cmd = knownFileExtensions[suffix];
	if (knownFileType && cmd.isEmpty()) {
		setFileName(new_filename);
		updateRecentFiles();		
	} else {
		setFileName("");
		editor->setPlainText(cmd.arg(new_filename));
	}

	fileChangedOnDisk(); // force cached autoReloadId to update
	refreshDocument();
	clearCurrentOutput();
}

void MainWindow::setFileName(const QString &filename)
{
	if (filename.isEmpty()) {
		this->fileName.clear();
		setWindowFilePath("untitled.scad");
		
		this->top_ctx.setDocumentPath(currentdir);
	} else {
		QFileInfo fileinfo(filename);
		this->fileName = fileinfo.exists() ? fileinfo.absoluteFilePath() : fileinfo.fileName();
		setWindowFilePath(this->fileName);

		QDir::setCurrent(fileinfo.dir().absolutePath());
		this->top_ctx.setDocumentPath(fileinfo.dir().absolutePath().toLocal8Bit().constData());
	}
	editorTopLevelChanged(editorDock->isFloating());
	consoleTopLevelChanged(consoleDock->isFloating());
}

void MainWindow::updateRecentFiles()
{
	// Check that the canonical file path exists - only update recent files
	// if it does. Should prevent empty list items on initial open etc.
	QFileInfo fileinfo(this->fileName);
	QString infoFileName = fileinfo.absoluteFilePath();
	QSettings settings; // already set up properly via main.cpp
	QStringList files = settings.value("recentFileList").toStringList();
	files.removeAll(infoFileName);
	files.prepend(infoFileName);
	while (files.size() > UIUtils::maxRecentFiles) files.removeLast();
	settings.setValue("recentFileList", files);

	foreach(QWidget *widget, QApplication::topLevelWidgets()) {
		MainWindow *mainWin = qobject_cast<MainWindow *>(widget);
		if (mainWin) {
			mainWin->updateRecentFileActions();
		}
	}
}

void MainWindow::updatedFps()
{
	bool fps_ok;
	double fps = this->e_fps->text().toDouble(&fps_ok);
	animate_timer->stop();
	if (fps_ok && fps > 0) {
		animate_timer->setSingleShot(false);
		animate_timer->setInterval(int(1000 / this->e_fps->text().toDouble()));
		animate_timer->start();
	}
}

void MainWindow::updateTVal()
{
	bool fps_ok;
	double fps = this->e_fps->text().toDouble(&fps_ok);
	if (fps_ok) {
		if (fps <= 0) {
			actionRenderPreview();
		} else {
			double s = this->e_fsteps->text().toDouble();
			double t = this->e_tval->text().toDouble() + 1/s;
			QString txt;
			txt.sprintf("%.5f", t >= 1.0 ? 0.0 : t);
			this->e_tval->setText(txt);
		}
	}
}

void MainWindow::refreshDocument()
{
	setCurrentOutput();
	if (!this->fileName.isEmpty()) {
		QFile file(this->fileName);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			PRINTB("Failed to open file %s: %s",
						 this->fileName.toLocal8Bit().constData() % file.errorString().toLocal8Bit().constData());
		}
		else {
			QTextStream reader(&file);
			reader.setCodec("UTF-8");
			QString text = reader.readAll();
			PRINTB("Loaded design '%s'.", this->fileName.toLocal8Bit().constData());
			if (editor->toPlainText() != text)
				editor->setPlainText(text);
		}
	}
	setCurrentOutput();
}

/*!
	compiles the design. Calls compileDone() if anything was compiled
*/
void MainWindow::compile(bool reload, bool forcedone)
{
	bool shouldcompiletoplevel = false;
	bool didcompile = false;

	// Reload checks the timestamp of the toplevel file and refreshes if necessary,
	if (reload) {
		// Refresh files if it has changed on disk
		if (fileChangedOnDisk() && checkEditorModified()) {
			shouldcompiletoplevel = true;
			refreshDocument();
		}
		// If the file hasn't changed, we might still need to compile it
		// if we haven't yet compiled the current text.
		else {
			QString current_doc = editor->toPlainText();
			if (current_doc != last_compiled_doc && last_compiled_doc.size() == 0) {
				shouldcompiletoplevel = true;
			}
		}
	}
	else {
		shouldcompiletoplevel = true;
	}

	if (!shouldcompiletoplevel && this->root_module && this->root_module->includesChanged()) {
		shouldcompiletoplevel = true;
	}

	if (shouldcompiletoplevel) {
		console->clear();
		if (editor->isContentModified()) saveBackup();
		compileTopLevelDocument();
		didcompile = true;
	}

	if (this->root_module) {
		if (this->root_module->handleDependencies()) {
			PRINTB("Module cache size: %d modules", ModuleCache::instance()->size());
			didcompile = true;
		}
	}

	// If we're auto-reloading, listen for a cascade of changes by starting a timer
	// if something changed _and_ there are any external dependencies
	if (reload && didcompile && this->root_module) {
		if (this->root_module->hasIncludes() ||
				this->root_module->usesLibraries()) {
			this->waitAfterReloadTimer->start();
			return;
		}
	}

	if (!reload && didcompile) {
		if (!animate_panel->isVisible()) {
			emit unhighlightLastError();
			if (!this->root_module) {
				emit highlightError( parser_error_pos );
			}
		}
	}

	compileDone(didcompile | forcedone);
}

void MainWindow::waitAfterReload()
{
	if (this->root_module->handleDependencies()) {
		this->waitAfterReloadTimer->start();
		return;
	}
	else {
		compile(true, true); // In case file itself or top-level includes changed during dependency updates
	}
}

void MainWindow::compileDone(bool didchange)
{
	const char *callslot;
	if (didchange) {
		instantiateRoot();
		callslot = afterCompileSlot;
	}
	else {
		callslot = "compileEnded";
	}

	this->procevents = false;
	QMetaObject::invokeMethod(this, callslot);
}

void MainWindow::compileEnded()
{
	clearCurrentOutput();
	GuiLocker::unlock();
	if (designActionAutoReload->isChecked()) autoReloadTimer->start();
}

void MainWindow::instantiateRoot()
{
	// Go on and instantiate root_node, then call the continuation slot

  // Invalidate renderers before we kill the CSG tree
	this->qglview->setRenderer(NULL);
	delete this->opencsgRenderer;
	this->opencsgRenderer = NULL;
	delete this->thrownTogetherRenderer;
	this->thrownTogetherRenderer = NULL;

	// Remove previous CSG tree
	delete this->absolute_root_node;
	this->absolute_root_node = NULL;

	this->root_raw_term.reset();
	this->root_norm_term.reset();

	delete this->root_chain;
	this->root_chain = NULL;

	this->highlight_terms.clear();
	delete this->highlights_chain;
	this->highlights_chain = NULL;

	this->background_terms.clear();
	delete this->background_chain;
	this->background_chain = NULL;

	this->root_node = NULL;
	this->tree.setRoot(NULL);

	if (this->root_module) {
		// Evaluate CSG tree
		PRINT("Compiling design (CSG Tree generation)...");
		if (this->procevents) QApplication::processEvents();

		AbstractNode::resetIndexCounter();

		// split these two lines - gcc 4.7 bug
		ModuleInstantiation mi = ModuleInstantiation( "group" );
		this->root_inst = mi;

		this->absolute_root_node = this->root_module->instantiate(&top_ctx, &this->root_inst, NULL);

		if (this->absolute_root_node) {
			// Do we have an explicit root node (! modifier)?
			if (!(this->root_node = find_root_tag(this->absolute_root_node))) {
				this->root_node = this->absolute_root_node;
			}
			// FIXME: Consider giving away ownership of root_node to the Tree, or use reference counted pointers
			this->tree.setRoot(this->root_node);
			// Dump the tree (to initialize caches).
			// FIXME: We shouldn't really need to do this explicitly..
			this->tree.getString(*this->root_node);
		}
	}

	if (!this->root_node) {
		if (parser_error_pos < 0) {
			PRINT("ERROR: Compilation failed! (no top level object found)");
		} else {
			PRINT("ERROR: Compilation failed!");
		}
		if (this->procevents) QApplication::processEvents();
	}
}

/*!
	Generates CSG tree for OpenCSG evaluation.
	Assumes that the design has been parsed and evaluated (this->root_node is set)
*/
void MainWindow::compileCSG(bool procevents)
{
	assert(this->root_node);
	PRINT("Compiling design (CSG Products generation)...");
	if (procevents) QApplication::processEvents();

	// Main CSG evaluation
	QTime t;
	t.start();

	this->progresswidget = new ProgressWidget(this);
	connect(this->progresswidget, SIGNAL(requestShow()), this, SLOT(showProgress()));

	progress_report_prep(this->root_node, report_func, this);
	try {
#ifdef ENABLE_CGAL
		GeometryEvaluator geomevaluator(this->tree);
#else
		// FIXME: Will we support this?
#endif
		CSGTermEvaluator csgrenderer(this->tree, &geomevaluator);
		if (procevents) QApplication::processEvents();
		this->root_raw_term = csgrenderer.evaluateCSGTerm(*root_node, highlight_terms, background_terms);
		if (!root_raw_term) {
			PRINT("ERROR: CSG generation failed! (no top level object found)");
		}
		GeometryCache::instance()->print();
#ifdef ENABLE_CGAL
		CGALCache::instance()->print();
#endif
		if (procevents) QApplication::processEvents();
	}
	catch (const ProgressCancelException &e) {
		PRINT("CSG generation cancelled.");
	}
	progress_report_fin();
	this->statusBar()->removeWidget(this->progresswidget);
	delete this->progresswidget;
	this->progresswidget = NULL;

	if (root_raw_term) {
		PRINT("Compiling design (CSG Products normalization)...");
		if (procevents) QApplication::processEvents();

		size_t normalizelimit = 2 * Preferences::inst()->getValue("advanced/openCSGLimit").toUInt();
		CSGTermNormalizer normalizer(normalizelimit);
		this->root_norm_term = normalizer.normalize(this->root_raw_term);
		if (this->root_norm_term) {
			this->root_chain = new CSGChain();
			this->root_chain->import(this->root_norm_term);
		}
		else {
			this->root_chain = NULL;
			PRINT("WARNING: CSG normalization resulted in an empty tree");
			if (procevents) QApplication::processEvents();
		}

		if (highlight_terms.size() > 0)
		{
			PRINTB("Compiling highlights (%d CSG Trees)...", highlight_terms.size());
			if (procevents) QApplication::processEvents();

			highlights_chain = new CSGChain();
			for (unsigned int i = 0; i < highlight_terms.size(); i++) {
				highlight_terms[i] = normalizer.normalize(highlight_terms[i]);
				highlights_chain->import(highlight_terms[i]);
			}
		}

		if (background_terms.size() > 0)
		{
			PRINTB("Compiling background (%d CSG Trees)...", background_terms.size());
			if (procevents) QApplication::processEvents();

			background_chain = new CSGChain();
			for (unsigned int i = 0; i < background_terms.size(); i++) {
				background_terms[i] = normalizer.normalize(background_terms[i]);
				background_chain->import(background_terms[i]);
			}
		}

		if (this->root_chain &&
				(this->root_chain->objects.size() >
				 Preferences::inst()->getValue("advanced/openCSGLimit").toUInt())) {
			PRINTB("WARNING: Normalized tree has %d elements!", this->root_chain->objects.size());
			PRINT("WARNING: OpenCSG rendering has been disabled.");
		}
		else {
			PRINTB("Normalized CSG tree has %d elements",
						 (this->root_chain ? this->root_chain->objects.size() : 0));
			this->opencsgRenderer = new OpenCSGRenderer(this->root_chain,
																									this->highlights_chain,
																									this->background_chain,
																									this->qglview->shaderinfo);
		}
		this->thrownTogetherRenderer = new ThrownTogetherRenderer(this->root_chain,
																															this->highlights_chain,
																															this->background_chain);
		PRINT("CSG generation finished.");
		int s = t.elapsed() / 1000;
		PRINTB("Total rendering time: %d hours, %d minutes, %d seconds", (s / (60*60)) % ((s / 60) % 60) % (s % 60));
		if (procevents) QApplication::processEvents();
	}
}

void MainWindow::actionUpdateCheck()
{
	if (AutoUpdater *updater =AutoUpdater::updater()) {
		updater->checkForUpdates();
	}
}

void MainWindow::actionNew()
{
	if (MainWindow::mdiMode) {
		new MainWindow(QString());
	} else {
		if (!maybeSave())
			return;

		setFileName("");
		editor->setPlainText("");
	}
}

void MainWindow::actionOpen()
{
	QFileInfo fileInfo = UIUtils::openFile(this);
	if (!fileInfo.exists()) {
	    return;
	}

	if (!MainWindow::mdiMode && !maybeSave()) {
	    return;
	}
	
	openFile(fileInfo.filePath());
}

void MainWindow::actionOpenRecent()
{
	if (!MainWindow::mdiMode && !maybeSave()) {
		return;
	}

	QAction *action = qobject_cast<QAction *>(sender());
	openFile(action->data().toString());
}

void MainWindow::clearRecentFiles()
{
	QSettings settings; // already set up properly via main.cpp
	QStringList files;
	settings.setValue("recentFileList", files);

	updateRecentFileActions();
}

void MainWindow::updateRecentFileActions()
{
    QStringList files = UIUtils::recentFiles();

    for (int i = 0; i < files.size(); ++i) {
	this->actionRecentFile[i]->setText(QFileInfo(files[i]).fileName());
	this->actionRecentFile[i]->setData(files[i]);
	this->actionRecentFile[i]->setVisible(true);
    }
    for (int i = files.size(); i < UIUtils::maxRecentFiles; ++i) {
	this->actionRecentFile[i]->setVisible(false);
    }
}

void MainWindow::show_examples()
{
        bool found_example = false;

        foreach (const QString &cat, UIUtils::exampleCategories()) {
		QFileInfoList examples = UIUtils::exampleFiles(cat);
                QMenu *menu = this->menuExamples->addMenu(cat);

                foreach(const QFileInfo &ex, examples) {
                        QAction *openAct = new QAction(ex.fileName(), this);
                        connect(openAct, SIGNAL(triggered()), this, SLOT(actionOpenExample()));
                        menu->addAction(openAct);
                        openAct->setData(ex.canonicalFilePath());
                        found_example = true;
                }
        }

        if (!found_example) {
                delete this->menuExamples;
                this->menuExamples = NULL;
        }
}

void MainWindow::actionOpenExample()
{
	if (!MainWindow::mdiMode && !maybeSave()) {
		return;
	}

	const QAction *action = qobject_cast<QAction *>(sender());
	if (action) {
		const QString path = action->data().toString();
		openFile(path);
	}
}

void MainWindow::writeBackup(QFile *file)
{
	// see MainWindow::saveBackup()
	file->resize(0);
	QTextStream writer(file);
	writer.setCodec("UTF-8");
	writer << this->editor->toPlainText();

	PRINTB("Saved backup file: %s", file->fileName().toLocal8Bit().constData());
}

void MainWindow::saveBackup()
{
	std::string path = PlatformUtils::backupPath();
	if ((!fs::exists(path)) && (!PlatformUtils::createBackupPath())) {
		PRINTB("WARNING: Cannot create backup path: %s", path);
		return;
	}

	QString backupPath = QString::fromStdString(path);
	if (!backupPath.endsWith("/")) backupPath.append("/");

	QString basename = "unsaved";
	if (!this->fileName.isEmpty()) {
		QFileInfo fileInfo = QFileInfo(this->fileName);
		basename = fileInfo.baseName();
	}

	if (!this->tempFile) {
		this->tempFile = new QTemporaryFile(backupPath.append(basename + "-backup-XXXXXXXX.scad"));
	}

	if ((!this->tempFile->isOpen()) && (! this->tempFile->open())) {
		PRINT("WARNING: Failed to create backup file");
		return;
	}
	return writeBackup(this->tempFile);
}

void MainWindow::actionSave()
{
	if (this->fileName.isEmpty()) {
		actionSaveAs();
	}
	else {
		if (!editor->isContentModified())
			return;
		setCurrentOutput();
		QFile file(this->fileName);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
			PRINTB("Failed to open file for writing: %s (%s)", 
			this->fileName.toLocal8Bit().constData() % file.errorString().toLocal8Bit().constData());
			QMessageBox::warning(this, windowTitle(), tr("Failed to open file for writing:\n %1 (%2)")
					.arg(this->fileName).arg(file.errorString()));
		}
		else {
			QTextStream writer(&file);
			writer.setCodec("UTF-8");
			writer << this->editor->toPlainText();
			PRINTB("Saved design '%s'.", this->fileName.toLocal8Bit().constData());
			this->editor->setContentModified(false);
		}
		clearCurrentOutput();
		updateRecentFiles();
	}
}

void MainWindow::actionSaveAs()
{
	QString new_filename = QFileDialog::getSaveFileName(this, "Save File",
			this->fileName.isEmpty()?"Untitled.scad":this->fileName,
			"OpenSCAD Designs (*.scad)");
	if (!new_filename.isEmpty()) {
		if (QFileInfo(new_filename).suffix().isEmpty()) {
			new_filename.append(".scad");

			// Manual overwrite check since Qt doesn't do it, when using the
			// defaultSuffix property
			QFileInfo info(new_filename);
			if (info.exists()) {
				if (QMessageBox::warning(this, windowTitle(),
						 tr("%1 already exists.\nDo you want to replace it?").arg(info.fileName()),
						 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
					return;
				}
			}
		}
		setFileName(new_filename);
		actionSave();
	}
}

void MainWindow::actionShowLibraryFolder()
{
	std::string path = PlatformUtils::userLibraryPath();
	if (!fs::exists(path)) {
		PRINTB("WARNING: Library path %s doesnt exist. Creating", path);
		if (!PlatformUtils::createUserLibraryPath()) {
			PRINTB("ERROR: Cannot create library path: %s",path);
		}
	}
	QString url = QString::fromStdString(path);
	//PRINTB("Opening file browser for %s", url.toStdString() );
	QDesktopServices::openUrl(QUrl::fromLocalFile(url));
}

void MainWindow::actionReload()
{
	if (checkEditorModified()) {
		fileChangedOnDisk(); // force cached autoReloadId to update
		refreshDocument();
	}
}

void MainWindow::pasteViewportTranslation()
{
	QString txt;
	txt.sprintf("[ %.2f, %.2f, %.2f ]", -qglview->cam.object_trans.x(), -qglview->cam.object_trans.y(), -qglview->cam.object_trans.z());
	this->editor->insert(txt);
}

void MainWindow::pasteViewportRotation()
{
	QString txt;
	txt.sprintf("[ %.2f, %.2f, %.2f ]",
		fmodf(360 - qglview->cam.object_rot.x() + 90, 360),
		fmodf(360 - qglview->cam.object_rot.y(), 360),
		fmodf(360 - qglview->cam.object_rot.z(), 360));
	this->editor->insert(txt);
}

void MainWindow::find()
{
	findTypeComboBox->setCurrentIndex(0);
	replaceInputField->hide();
	replaceButton->hide();
	replaceAllButton->hide();
	find_panel->show();
	findInputField->setText(editor->selectedText());
	findInputField->setFocus();
	findInputField->selectAll();
}

void MainWindow::findString(QString textToFind)
{
	editor->find(textToFind, false, false);
}

void MainWindow::findAndReplace()
{
	findTypeComboBox->setCurrentIndex(1);
	replaceInputField->show();
	replaceButton->show();
	replaceAllButton->show();
	find_panel->show();
	findInputField->setText(editor->selectedText());
	findInputField->setFocus();
	findInputField->selectAll();
}

void MainWindow::selectFindType(int type) {
	if (type == 0) find();
	if (type == 1) findAndReplace();
}

void MainWindow::replace() {
	this->editor->replaceSelectedText(this->replaceInputField->text());
	this->editor->find(this->findInputField->text());
}

void MainWindow::replaceAll() {
	while (this->editor->find(this->findInputField->text(), true)) {
		this->editor->replaceSelectedText(this->replaceInputField->text());
	}
}

void MainWindow::findNext()
{
	editor->find(this->findInputField->text(), true);
}

void MainWindow::findPrev()
{
	editor->find(this->findInputField->text(), true, true);
}

bool MainWindow::eventFilter(QObject* obj, QEvent *event)
{
    if (obj == find_panel)
    {
        if (event->type() == QEvent::KeyPress)
        {
            QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape)
            {
				find_panel->hide();
				return true;
            }
        }
        return false;
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::updateTemporalVariables()
{
	this->top_ctx.set_variable("$t", Value(this->e_tval->text().toDouble()));

	Value::VectorType vpt;
	vpt.push_back(Value(-qglview->cam.object_trans.x()));
	vpt.push_back(Value(-qglview->cam.object_trans.y()));
	vpt.push_back(Value(-qglview->cam.object_trans.z()));
	this->top_ctx.set_variable("$vpt", Value(vpt));

	Value::VectorType vpr;
	vpr.push_back(Value(fmodf(360 - qglview->cam.object_rot.x() + 90, 360)));
	vpr.push_back(Value(fmodf(360 - qglview->cam.object_rot.y(), 360)));
	vpr.push_back(Value(fmodf(360 - qglview->cam.object_rot.z(), 360)));
	top_ctx.set_variable("$vpr", Value(vpr));

	top_ctx.set_variable("$vpd", Value(qglview->cam.viewer_distance));
}


/*!
 * Update the viewport camera by evaluating the special variables. If they
 * are assigned on top-level, the values are used to change the camera
 * rotation, translation and distance. 
 */
void MainWindow::updateCamera()
{
	if (!root_module)
		return;
	
	bool camera_set = false;

	Camera cam(qglview->cam);
	cam.gimbalDefaultTranslate();
	double tx = cam.object_trans.x();
	double ty = cam.object_trans.y();
	double tz = cam.object_trans.z();
	double rx = cam.object_rot.x();
	double ry = cam.object_rot.y();
	double rz = cam.object_rot.z();
	double d = cam.viewer_distance;

	ModuleContext mc(&top_ctx, NULL);
	mc.initializeModule(*root_module);

	BOOST_FOREACH(const Assignment &a, root_module->scope.assignments) {
		double x, y, z;
		if ("$vpr" == a.first) {
			const Value vpr = a.second.get()->evaluate(&mc);
			if (vpr.getVec3(x, y, z)) {
				rx = x;
				ry = y;
				rz = z;
				camera_set = true;
			}
		} else if ("$vpt" == a.first) {
			const Value vpt = a.second.get()->evaluate(&mc);
			if (vpt.getVec3(x, y, z)) {
				tx = x;
				ty = y;
				tz = z;
				camera_set = true;
			}
		} else if ("$vpd" == a.first) {
			const Value vpd = a.second.get()->evaluate(&mc);
			if (vpd.type() == Value::NUMBER) {
				d = vpd.toDouble();
				camera_set = true;
			}
		}
	}

	if (camera_set) {
		std::vector<double> params;
		params.push_back(tx);
		params.push_back(ty);
		params.push_back(tz);
		params.push_back(rx);
		params.push_back(ry);
		params.push_back(rz);
		params.push_back(d);
		qglview->cam.setup(params);
		qglview->cam.gimbalDefaultTranslate();
		qglview->updateGL();
	}
}

/*!
	Returns true if the current document is a file on disk and that file has new content.
	Returns false if a file on disk has disappeared or if we haven't yet saved.
*/
bool MainWindow::fileChangedOnDisk()
{
	if (!this->fileName.isEmpty()) {
		struct stat st;
		memset(&st, 0, sizeof(struct stat));
		bool valid = (stat(this->fileName.toLocal8Bit(), &st) == 0);
		// If file isn't there, just return and use current editor text
		if (!valid) return false;

		std::string newid = str(boost::format("%x.%x") % st.st_mtime % st.st_size);

		if (newid != this->autoReloadId) {
			this->autoReloadId = newid;
			return true;
		}
	}
	return false;
}

/*!
	Returns true if anything was compiled.
*/
void MainWindow::compileTopLevelDocument()
{
	updateTemporalVariables();
	resetPrintedDeprecations();

	this->last_compiled_doc = editor->toPlainText();

	std::string fulltext =
		std::string(this->last_compiled_doc.toLocal8Bit().constData()) +
		"\n" + commandline_commands;

	delete this->root_module;
	this->root_module = NULL;

	this->root_module = parse(fulltext.c_str(),
	this->fileName.isEmpty() ? "" :
	QFileInfo(this->fileName).absolutePath().toLocal8Bit(), false);

	updateCamera();
}

void MainWindow::checkAutoReload()
{
	if (!this->fileName.isEmpty()) {
		actionReloadRenderPreview();
	}
}

void MainWindow::autoReloadSet(bool on)
{
	QSettings settings;
	settings.setValue("design/autoReload",designActionAutoReload->isChecked());
	if (on) {
		autoReloadTimer->start(200);
	} else {
		autoReloadTimer->stop();
	}
}

bool MainWindow::checkEditorModified()
{
	if (editor->isContentModified()) {
		QMessageBox::StandardButton ret;
		ret = QMessageBox::warning(this, "Application",
				"The document has been modified.\n"
				"Do you really want to reload the file?",
				QMessageBox::Yes | QMessageBox::No);
		if (ret != QMessageBox::Yes) {
			designActionAutoReload->setChecked(false);
			return false;
		}
	}
	return true;
}

void MainWindow::actionReloadRenderPreview()
{
	if (GuiLocker::isLocked()) return;
	GuiLocker::lock();
	autoReloadTimer->stop();
	setCurrentOutput();

	// PRINT("Parsing design (AST generation)...");
	// QApplication::processEvents();
	this->afterCompileSlot = "csgReloadRender";
	this->procevents = true;
	compile(true);
}

void MainWindow::csgReloadRender()
{
	if (this->root_node) compileCSG(true);

	// Go to non-CGAL view mode
	if (viewActionThrownTogether->isChecked()) {
		viewModeThrownTogether();
	}
	else {
#ifdef ENABLE_OPENCSG
		viewModePreview();
#else
		viewModeThrownTogether();
#endif
	}
	compileEnded();
}

void MainWindow::actionRenderPreview()
{
	if (GuiLocker::isLocked()) return;
	GuiLocker::lock();
	autoReloadTimer->stop();
	setCurrentOutput();

	PRINT("Parsing design (AST generation)...");
	QApplication::processEvents();
	this->afterCompileSlot = "csgRender";
	this->procevents = !viewActionAnimate->isChecked();
	compile(false);
}

void MainWindow::csgRender()
{
	if (this->root_node) compileCSG(!viewActionAnimate->isChecked());

	// Go to non-CGAL view mode
	if (viewActionThrownTogether->isChecked()) {
		viewModeThrownTogether();
	}
	else {
#ifdef ENABLE_OPENCSG
		viewModePreview();
#else
		viewModeThrownTogether();
#endif
	}

	if (viewActionAnimate->isChecked() && e_dump->isChecked()) {
		QImage img = this->qglview->grabFrameBuffer();
		QString filename;
		double s = this->e_fsteps->text().toDouble();
		double t = this->e_tval->text().toDouble();
		filename.sprintf("frame%05d.png", int(round(s*t)));
		img.save(filename, "PNG");
	}

	compileEnded();
}

#ifdef ENABLE_CGAL

void MainWindow::actionRender()
{
	if (GuiLocker::isLocked()) return;
	GuiLocker::lock();
	autoReloadTimer->stop();
	setCurrentOutput();

	PRINT("Parsing design (AST generation)...");
	QApplication::processEvents();
	this->afterCompileSlot = "cgalRender";
	this->procevents = true;
	compile(false);
}

void MainWindow::cgalRender()
{
	if (!this->root_module || !this->root_node) {
        compileEnded();
		return;
	}

	this->qglview->setRenderer(NULL);
	delete this->cgalRenderer;
	this->cgalRenderer = NULL;
	this->root_geom.reset();

	PRINT("Rendering Polygon Mesh using CGAL...");

	this->progresswidget = new ProgressWidget(this);
	connect(this->progresswidget, SIGNAL(requestShow()), this, SLOT(showProgress()));

	progress_report_prep(this->root_node, report_func, this);

	this->cgalworker->start(this->tree);
}

void MainWindow::actionRenderDone(shared_ptr<const Geometry> root_geom)
{
	progress_report_fin();

	if (root_geom) {
		GeometryCache::instance()->print();
#ifdef ENABLE_CGAL
		CGALCache::instance()->print();
#endif

		int s = this->progresswidget->elapsedTime() / 1000;
		PRINTB("Total rendering time: %d hours, %d minutes, %d seconds", (s / (60*60)) % ((s / 60) % 60) % (s % 60));
			
		if (const CGAL_Nef_polyhedron *N = dynamic_cast<const CGAL_Nef_polyhedron *>(root_geom.get())) {
			if (!N->isEmpty()) {
				if (N->getDimension() == 3) {
					PRINT("   Top level object is a 3D object:");
					PRINTB("   Simple:     %6s", (N->p3->is_simple() ? "yes" : "no"));
					PRINTB("   Vertices:   %6d", N->p3->number_of_vertices());
					PRINTB("   Halfedges:  %6d", N->p3->number_of_halfedges());
					PRINTB("   Edges:      %6d", N->p3->number_of_edges());
					PRINTB("   Halffacets: %6d", N->p3->number_of_halffacets());
					PRINTB("   Facets:     %6d", N->p3->number_of_facets());
					PRINTB("   Volumes:    %6d", N->p3->number_of_volumes());
				}
			}
		}
		else if (const PolySet *ps = dynamic_cast<const PolySet *>(root_geom.get())) {
			assert(ps->getDimension() == 3);
			PRINT("   Top level object is a 3D object:");
			PRINTB("   Facets:     %6d", ps->numPolygons());
		} else if (const Polygon2d *poly = dynamic_cast<const Polygon2d *>(root_geom.get())) {
			PRINT("   Top level object is a 2D object:");
			PRINTB("   Contours:     %6d", poly->outlines().size());
		} else {
			assert(false && "Unknown geometry type");
		}
		PRINT("Rendering finished.");

		this->root_geom = root_geom;
		this->cgalRenderer = new CGALRenderer(root_geom);
		// Go to CGAL view mode
		if (viewActionWireframe->isChecked()) viewModeWireframe();
		else viewModeSurface();
	}
	else {
		PRINT("WARNING: No top level geometry to render");
	}

	this->statusBar()->removeWidget(this->progresswidget);
	delete this->progresswidget;
	this->progresswidget = NULL;
	compileEnded();
}

#endif /* ENABLE_CGAL */

void MainWindow::actionDisplayAST()
{
	setCurrentOutput();
	QTextEdit *e = new QTextEdit(this);
	e->setWindowFlags(Qt::Window);
	e->setTabStopWidth(30);
	e->setWindowTitle("AST Dump");
	e->setReadOnly(true);
	if (root_module) {
		e->setPlainText(QString::fromLocal8Bit(root_module->dump("", "").c_str()));
	} else {
		e->setPlainText("No AST to dump. Please try compiling first...");
	}
	e->show();
	e->resize(600, 400);
	clearCurrentOutput();
}

void MainWindow::actionDisplayCSGTree()
{
	setCurrentOutput();
	QTextEdit *e = new QTextEdit(this);
	e->setWindowFlags(Qt::Window);
	e->setTabStopWidth(30);
	e->setWindowTitle("CSG Tree Dump");
	e->setReadOnly(true);
	if (this->root_node) {
		e->setPlainText(QString::fromLocal8Bit(this->tree.getString(*this->root_node).c_str()));
	} else {
		e->setPlainText("No CSG to dump. Please try compiling first...");
	}
	e->show();
	e->resize(600, 400);
	clearCurrentOutput();
}

void MainWindow::actionDisplayCSGProducts()
{
	setCurrentOutput();
	QTextEdit *e = new QTextEdit(this);
	e->setWindowFlags(Qt::Window);
	e->setTabStopWidth(30);
	e->setWindowTitle("CSG Products Dump");
	e->setReadOnly(true);
	e->setPlainText(QString("\nCSG before normalization:\n%1\n\n\nCSG after normalization:\n%2\n\n\nCSG rendering chain:\n%3\n\n\nHighlights CSG rendering chain:\n%4\n\n\nBackground CSG rendering chain:\n%5\n")
									
	.arg(root_raw_term ? QString::fromLocal8Bit(root_raw_term->dump().c_str()) : "N/A",
	root_norm_term ? QString::fromLocal8Bit(root_norm_term->dump().c_str()) : "N/A",
	this->root_chain ? QString::fromLocal8Bit(this->root_chain->dump().c_str()) : "N/A",
	highlights_chain ? QString::fromLocal8Bit(highlights_chain->dump().c_str()) : "N/A",
	background_chain ? QString::fromLocal8Bit(background_chain->dump().c_str()) : "N/A"));
	
	e->show();
	e->resize(600, 400);
	clearCurrentOutput();
}

void MainWindow::actionCheckValidity() {
	if (GuiLocker::isLocked()) return;
	GuiLocker lock;
#ifdef ENABLE_CGAL
	setCurrentOutput();

	if (!this->root_geom) {
		PRINT("Nothing to validate! Try building first (press F6).");
		clearCurrentOutput();
		return;
	}

	if (this->root_geom->getDimension() != 3) {
		PRINT("Current top level object is not a 3D object.");
		clearCurrentOutput();
		return;
	}

	bool valid = false;
	if (const CGAL_Nef_polyhedron *N = dynamic_cast<const CGAL_Nef_polyhedron *>(this->root_geom.get()))
		valid = N->p3->is_valid();

	PRINTB("   Valid:      %6s", (valid ? "yes" : "no"));
	clearCurrentOutput();
#endif /* ENABLE_CGAL */
}

#ifdef ENABLE_CGAL
void MainWindow::actionExport(export_type_e export_type, const char *type_name, const char *suffix)
#else
void MainWindow::actionExport(export_type_e, QString, QString)
#endif
{
	if (GuiLocker::isLocked()) return;
	GuiLocker lock;
#ifdef ENABLE_CGAL
	setCurrentOutput();

	if (!this->root_geom) {
		PRINT("Nothing to export! Try building first (press F6).");
		clearCurrentOutput();
		return;
	}

	if (this->root_geom->getDimension() != 3) {
		PRINT("Current top level object is not a 3D object.");
		clearCurrentOutput();
		return;
	}

	if (this->root_geom->isEmpty()) {
		PRINT("Current top level object is empty.");
		clearCurrentOutput();
		return;
	}

	const CGAL_Nef_polyhedron *N = dynamic_cast<const CGAL_Nef_polyhedron *>(this->root_geom.get());
	if (N && !N->p3->is_simple()) {
	 	PRINT("Warning: Object may not be a valid 2-manifold and may need repair! See http://en.wikibooks.org/wiki/OpenSCAD_User_Manual/STL_Import_and_Export");
	}

	QString title = QString("Export %1 File").arg(type_name);
	QString filter = QString("%1 Files (*%2)").arg(type_name, suffix);
	QString filename = this->fileName.isEmpty() ? QString("Untitled") + suffix : QFileInfo(this->fileName).baseName() + suffix;
	QString export_filename = QFileDialog::getSaveFileName(this, title, filename, filter);
	if (export_filename.isEmpty()) {
		PRINTB("No filename specified. %s export aborted.", type_name);
		clearCurrentOutput();
		return;
	}

	enum FileFormat format = (enum FileFormat)-1;
	switch (export_type) {
	case EXPORT_TYPE_STL: format = OPENSCAD_STL; break;
	case EXPORT_TYPE_OFF: format = OPENSCAD_OFF; break;
	case EXPORT_TYPE_AMF: format = OPENSCAD_AMF; break;
	default:
		assert(false && "Unknown export type");
		break;
	}
	exportFileByName(this->root_geom.get(), format, export_filename.toUtf8(),
		export_filename.toLocal8Bit().constData());
	PRINTB("%s export finished.", type_name);

	clearCurrentOutput();
#endif /* ENABLE_CGAL */
}

void MainWindow::actionExportSTL()
{
	actionExport(EXPORT_TYPE_STL, "STL", ".stl");
}

void MainWindow::actionExportOFF()
{
	actionExport(EXPORT_TYPE_OFF, "OFF", ".off");
}

void MainWindow::actionExportAMF()
{
	actionExport(EXPORT_TYPE_AMF, "AMF", ".amf");
}

QString MainWindow::get2dExportFilename(QString format, QString extension) {
	setCurrentOutput();

	if (!this->root_geom) {
		PRINT("Nothing to export! Try building first (press F6).");
		clearCurrentOutput();
		return QString();
	}

	if (this->root_geom->getDimension() != 2) {
		PRINT("Current top level object is not a 2D object.");
		clearCurrentOutput();
		return QString();
	}

	QString caption = QString("Export %1 File").arg(format);
	QString suggestion = this->fileName.isEmpty()
		? QString("Untitled%1").arg(extension)
		: QFileInfo(this->fileName).baseName() + extension;
	QString filter = QString("%1 Files (*%2)").arg(format, extension);
	QString exportFilename = QFileDialog::getSaveFileName(this, caption, suggestion, filter);
	if (exportFilename.isEmpty()) {
		PRINT("No filename specified. DXF export aborted.");
		clearCurrentOutput();
		return QString();
	}
	
	return exportFilename;
}

void MainWindow::actionExportDXF()
{
#ifdef ENABLE_CGAL
	QString dxf_filename = get2dExportFilename("DXF", ".dxf");
	if (dxf_filename.isEmpty()) {
		return;
	}
	exportFileByName(this->root_geom.get(), OPENSCAD_DXF, dxf_filename.toUtf8(),
		dxf_filename.toLocal8Bit().constData());
	PRINT("DXF export finished.");

	clearCurrentOutput();
#endif /* ENABLE_CGAL */
}

void MainWindow::actionExportSVG()
{
	QString svg_filename = get2dExportFilename("SVG", ".svg");
	if (svg_filename.isEmpty()) {
		return;
	}
	exportFileByName(this->root_geom.get(), OPENSCAD_SVG, svg_filename.toUtf8(),
		svg_filename.toLocal8Bit().constData());
	PRINT("SVG export finished.");

	clearCurrentOutput();
}

void MainWindow::actionExportCSG()
{
	setCurrentOutput();

	if (!this->root_node) {
		PRINT("Nothing to export. Please try compiling first...");
		clearCurrentOutput();
		return;
	}

	QString csg_filename = QFileDialog::getSaveFileName(this, "Export CSG File",
	    this->fileName.isEmpty() ? "Untitled.csg" : QFileInfo(this->fileName).baseName()+".csg",
	    "CSG Files (*.csg)");
	
	if (csg_filename.isEmpty()) {
		PRINT("No filename specified. CSG export aborted.");
		clearCurrentOutput();
		return;
	}

	std::ofstream fstream(csg_filename.toLocal8Bit());
	if (!fstream.is_open()) {
		PRINTB("Can't open file \"%s\" for export", csg_filename.toLocal8Bit().constData());
	}
	else {
		fstream << this->tree.getString(*this->root_node) << "\n";
		fstream.close();
		PRINT("CSG export finished.");
	}

	clearCurrentOutput();
}

void MainWindow::actionExportImage()
{
	setCurrentOutput();

	QString img_filename = QFileDialog::getSaveFileName(this,
			"Export Image", "", "PNG Files (*.png)");
	if (img_filename.isEmpty()) {
		PRINT("No filename specified. Image export aborted.");
	} else {
		qglview->save(img_filename.toLocal8Bit().constData());
	}
	clearCurrentOutput();
	return;
}

void MainWindow::actionFlushCaches()
{
	GeometryCache::instance()->clear();
#ifdef ENABLE_CGAL
	CGALCache::instance()->clear();
#endif
	dxf_dim_cache.clear();
	dxf_cross_cache.clear();
	ModuleCache::instance()->clear();
	FontCache::instance()->clear();
}

void MainWindow::viewModeActionsUncheck()
{
	viewActionPreview->setChecked(false);
#ifdef ENABLE_CGAL
	viewActionSurfaces->setChecked(false);
	viewActionWireframe->setChecked(false);
#endif
	viewActionThrownTogether->setChecked(false);
}

#ifdef ENABLE_OPENCSG

/*!
	Go to the OpenCSG view mode.
	Falls back to thrown together mode if OpenCSG is not available
*/
void MainWindow::viewModePreview()
{
	if (this->qglview->hasOpenCSGSupport()) {
		viewModeActionsUncheck();
		viewActionPreview->setChecked(true);
		this->qglview->setRenderer(this->opencsgRenderer ? (Renderer *)this->opencsgRenderer : (Renderer *)this->thrownTogetherRenderer);
		this->qglview->updateColorScheme();
		this->qglview->updateGL();
	} else {
		viewModeThrownTogether();
	}
}

#endif /* ENABLE_OPENCSG */

#ifdef ENABLE_CGAL

void MainWindow::viewModeSurface()
{
	viewModeActionsUncheck();
	viewActionSurfaces->setChecked(true);
	this->qglview->setShowFaces(true);
	this->qglview->setRenderer(this->cgalRenderer);
	this->qglview->updateColorScheme();
	this->qglview->updateGL();
}

void MainWindow::viewModeWireframe()
{
	viewModeActionsUncheck();
	viewActionWireframe->setChecked(true);
	this->qglview->setShowFaces(false);
	this->qglview->setRenderer(this->cgalRenderer);
	this->qglview->updateColorScheme();
	this->qglview->updateGL();
}

#endif /* ENABLE_CGAL */

void MainWindow::viewModeThrownTogether()
{
	viewModeActionsUncheck();
	viewActionThrownTogether->setChecked(true);
	this->qglview->setRenderer(this->thrownTogetherRenderer);
	this->qglview->updateColorScheme();
	this->qglview->updateGL();
}

void MainWindow::viewModeShowEdges()
{
	QSettings settings;
	settings.setValue("view/showEdges",viewActionShowEdges->isChecked());
	this->qglview->setShowEdges(viewActionShowEdges->isChecked());
	this->qglview->updateGL();
}

void MainWindow::viewModeShowAxes()
{
	QSettings settings;
	settings.setValue("view/showAxes",viewActionShowAxes->isChecked());
	this->qglview->setShowAxes(viewActionShowAxes->isChecked());
	this->qglview->updateGL();
}

void MainWindow::viewModeShowCrosshairs()
{
	QSettings settings;
	settings.setValue("view/showCrosshairs",viewActionShowCrosshairs->isChecked());
	this->qglview->setShowCrosshairs(viewActionShowCrosshairs->isChecked());
	this->qglview->updateGL();
}

void MainWindow::viewModeAnimate()
{
	if (viewActionAnimate->isChecked()) {
		animate_panel->show();
		actionRenderPreview();
		updatedFps();
	} else {
		animate_panel->hide();
		animate_timer->stop();
	}
}

void MainWindow::animateUpdateDocChanged()
{
	QString current_doc = editor->toPlainText(); 
	if (current_doc != last_compiled_doc)
		animateUpdate();
}

void MainWindow::animateUpdate()
{
	if (animate_panel->isVisible()) {
		bool fps_ok;
		double fps = this->e_fps->text().toDouble(&fps_ok);
		if (fps_ok && fps <= 0 && !animate_timer->isActive()) {
			animate_timer->stop();
			animate_timer->setSingleShot(true);
			animate_timer->setInterval(50);
			animate_timer->start();
		}
	}
}

void MainWindow::viewAngleTop()
{
	qglview->cam.object_rot << 90,0,0;
	this->qglview->updateGL();
}

void MainWindow::viewAngleBottom()
{
	qglview->cam.object_rot << 270,0,0;
	this->qglview->updateGL();
}

void MainWindow::viewAngleLeft()
{
	qglview->cam.object_rot << 0,0,90;
	this->qglview->updateGL();
}

void MainWindow::viewAngleRight()
{
	qglview->cam.object_rot << 0,0,270;
	this->qglview->updateGL();
}

void MainWindow::viewAngleFront()
{
	qglview->cam.object_rot << 0,0,0;
	this->qglview->updateGL();
}

void MainWindow::viewAngleBack()
{
	qglview->cam.object_rot << 0,0,180;
	this->qglview->updateGL();
}

void MainWindow::viewAngleDiagonal()
{
	qglview->cam.object_rot << 35,0,-25;
	this->qglview->updateGL();
}

void MainWindow::viewCenter()
{
	qglview->cam.object_trans << 0,0,0;
	this->qglview->updateGL();
}

void MainWindow::viewPerspective()
{
	QSettings settings;
	settings.setValue("view/orthogonalProjection",false);
	viewActionPerspective->setChecked(true);
	viewActionOrthogonal->setChecked(false);
	this->qglview->setOrthoMode(false);
	this->qglview->updateGL();
}

void MainWindow::viewOrthogonal()
{
	QSettings settings;
	settings.setValue("view/orthogonalProjection",true);
	viewActionPerspective->setChecked(false);
	viewActionOrthogonal->setChecked(true);
	this->qglview->setOrthoMode(true);
	this->qglview->updateGL();
}

void MainWindow::viewResetView()
{
	this->qglview->resetView();
	this->qglview->updateGL();
}

void MainWindow::viewAll()
{
	this->qglview->viewAll();
	this->qglview->updateGL();
}

void MainWindow::on_editorDock_visibilityChanged(bool visible)
{
	if (isClosing) {
		return;
	}
	QSettings settings;
	settings.setValue("view/hideEditor", !visible);
	editActionHide->setChecked(!visible);
	editorTopLevelChanged(editorDock->isFloating());
}

void MainWindow::on_consoleDock_visibilityChanged(bool visible)
{
	if (isClosing) {
		return;
	}
	QSettings settings;
	settings.setValue("view/hideConsole", !visible);
	viewActionHide->setChecked(!visible);
	consoleTopLevelChanged(consoleDock->isFloating());
}

void MainWindow::editorTopLevelChanged(bool topLevel)
{
	setDockWidgetTitle(editorDock, QString("Editor"), topLevel);
}

void MainWindow::consoleTopLevelChanged(bool topLevel)
{
	setDockWidgetTitle(consoleDock, QString("Console"), topLevel);
}

void MainWindow::setDockWidgetTitle(QDockWidget *dockWidget, QString prefix, bool topLevel)
{
	QString title(prefix);
	if (topLevel) {
		const QFileInfo fileInfo(windowFilePath());
		title += " (" + fileInfo.fileName() + ")";
	}
	dockWidget->setWindowTitle(title);
}

void MainWindow::hideEditor()
{
	if (editActionHide->isChecked()) {
		editorDock->close();
	} else {
		editorDock->show();
	}
}

void MainWindow::hideConsole()
{
	if (viewActionHide->isChecked()) {
		consoleDock->hide();
	} else {
		consoleDock->show();
	}
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasUrls())
		event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
	setCurrentOutput();
	const QList<QUrl> urls = event->mimeData()->urls();
	for (int i = 0; i < urls.size(); i++) {
		if (urls[i].scheme() != "file")
			continue;
		
		handleFileDrop(urls[i].toLocalFile());
	}
	clearCurrentOutput();
}

void MainWindow::handleFileDrop(const QString &filename)
{
	const QFileInfo fileInfo(filename);
	const QString suffix = fileInfo.suffix().toLower();
	const QString cmd = knownFileExtensions[suffix];
	if (cmd.isEmpty()) {
		if (!MainWindow::mdiMode && !maybeSave()) {
			return;
		}
		openFile(filename);
	} else {
		editor->insert(cmd.arg(filename));
	}
}

void MainWindow::helpAbout()
{
	qApp->setWindowIcon(QApplication::windowIcon());
	AboutDialog *dialog = new AboutDialog(this);
	dialog->exec();
	//QMessageBox::information(this, "About OpenSCAD", QString(helptitle) + QString(copyrighttext));
}

void MainWindow::helpHomepage()
{
        UIUtils::openHomepageURL();
}

void MainWindow::helpManual()
{
        UIUtils::openUserManualURL();
}

void MainWindow::helpLibrary()
{
    if (!this->library_info_dialog) {
        QString rendererInfo(qglview->getRendererInfo().c_str());
        LibraryInfoDialog *dialog = new LibraryInfoDialog(rendererInfo);
        this->library_info_dialog = dialog;
    }
    this->library_info_dialog->show();
}

void MainWindow::helpFontInfo()
{
	if (!this->font_list_dialog) {
		FontListDialog *dialog = new FontListDialog();
		this->font_list_dialog = dialog;
	}
	this->font_list_dialog->update_font_list();
	this->font_list_dialog->show();
}

/*!
	FIXME: In MDI mode, should this be called on both reload functions?
 */
bool MainWindow::maybeSave()
{
	if (editor->isContentModified()) {
		QMessageBox::StandardButton ret;
		QMessageBox box(this);
		box.setText("The document has been modified.");
		box.setInformativeText("Do you want to save your changes?");
		box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
		box.setDefaultButton(QMessageBox::Save);
		box.setIcon(QMessageBox::Warning);
		box.setWindowModality(Qt::ApplicationModal);
#ifdef Q_OS_MAC
		// Cmd-D is the standard shortcut for this button on Mac
		box.button(QMessageBox::Discard)->setShortcut(QKeySequence("Ctrl+D"));
		box.button(QMessageBox::Discard)->setShortcutEnabled(true);
#endif
		ret = (QMessageBox::StandardButton) box.exec();

		if (ret == QMessageBox::Save) {
			actionSave();
			// Returns false on failed save
			return !editor->isContentModified();
		}
		else if (ret == QMessageBox::Cancel) {
			return false;
		}
	}
	return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (maybeSave()) {
		QSettings settings;
		settings.setValue("window/size", size());
		settings.setValue("window/position", pos());
		settings.setValue("window/state", saveState());
		if (this->tempFile) {
			delete this->tempFile;
			this->tempFile = NULL;
		}
		isClosing = true;
		event->accept();
	} else {
		event->ignore();
	}
}

void MainWindow::preferences()
{
	Preferences::inst()->show();
	Preferences::inst()->activateWindow();
	Preferences::inst()->raise();
}

void MainWindow::setColorScheme(const QString &scheme)
{
	RenderSettings::inst()->colorscheme = scheme.toStdString();
	this->qglview->setColorScheme(scheme.toStdString());
	this->qglview->updateGL();
}

void MainWindow::setFont(const QString &family, uint size)
{
	QFont font;
	if (!family.isEmpty()) font.setFamily(family);
	else font.setFixedPitch(true);
	if (size > 0)	font.setPointSize(size);
	font.setStyleHint(QFont::TypeWriter);
	editor->setFont(font);
}

void MainWindow::quit()
{
	QCloseEvent ev;
	QApplication::sendEvent(QApplication::instance(), &ev);
	if (ev.isAccepted()) QApplication::instance()->quit();
  // FIXME: Cancel any CGAL calculations
#ifdef Q_OS_MAC
	CocoaUtils::endApplication();
#endif
}

void MainWindow::consoleOutput(const std::string &msg, void *userdata)
{
	// Invoke the append function in the main thread in case the output
  // originates in a worker thread.
	MainWindow *thisp = static_cast<MainWindow*>(userdata);
	QMetaObject::invokeMethod(thisp->console, "append", Qt::QueuedConnection,
	Q_ARG(QString, QString::fromLocal8Bit(msg.c_str())));
	if (thisp->procevents) QApplication::processEvents();
}

void MainWindow::setCurrentOutput()
{
	set_output_handler(&MainWindow::consoleOutput, this);
}

void MainWindow::clearCurrentOutput()
{
	set_output_handler(NULL, NULL);
}

void MainWindow::openCSGSettingsChanged()
{
#ifdef ENABLE_OPENCSG
	OpenCSG::setOption(OpenCSG::AlgorithmSetting, Preferences::inst()->getValue("advanced/forceGoldfeather").toBool() ? 
	OpenCSG::Goldfeather : OpenCSG::Automatic);
#endif
}
