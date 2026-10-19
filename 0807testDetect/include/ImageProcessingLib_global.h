#pragma once

#include <QtCore/qglobal.h>

#ifndef BUILD_STATIC
# if defined(IMAGEPROCESSINGLIB_LIB)
#  define IMAGEPROCESSINGLIB_EXPORT Q_DECL_EXPORT
# else
#  define IMAGEPROCESSINGLIB_EXPORT Q_DECL_IMPORT
# endif
#else
# define IMAGEPROCESSINGLIB_EXPORT
#endif
