#include "openglwindow.h"

OpenGLWindow::OpenGLWindow(QWindow* parent)
    : OutputWindow(parent)
{
	QSurfaceFormat format;
	format.setVersion(3, 2);
	format.setProfile(QSurfaceFormat::CoreProfile);
	format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);

	setSurfaceType(QWindow::OpenGLSurface);
	setFormat(format);
}
