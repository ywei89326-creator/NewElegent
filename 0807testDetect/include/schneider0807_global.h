#pragma once

#include <QtCore/qglobal.h>

#ifndef BUILD_STATIC
# if defined(SCHNEIDER0807_LIB)
#  define SCHNEIDER0807_EXPORT Q_DECL_EXPORT
# else
#  define SCHNEIDER0807_EXPORT Q_DECL_IMPORT
# endif
#else
# define SCHNEIDER0807_EXPORT
#endif
