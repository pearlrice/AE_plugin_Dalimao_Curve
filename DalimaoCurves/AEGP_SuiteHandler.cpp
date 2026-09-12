/*
 * AEGP_SuiteHandler.cpp - Adobe SDK implementation
 * Must be compiled into the plugin (provides constructor/destructor/methods).
 * MissingSuiteError() is defined in DalimaoCurves.cpp.
 */

#include "pch.h"

#include <AEGP_SuiteHandler.h>
#include <AE_Macros.h>

AEGP_SuiteHandler::AEGP_SuiteHandler(const SPBasicSuite *pica_basicP) :
	i_pica_basicP(pica_basicP)
{
	AEFX_CLR_STRUCT(i_suites);

	if (!i_pica_basicP) {
		MissingSuiteError();
	}
}

AEGP_SuiteHandler::~AEGP_SuiteHandler()
{
	ReleaseAllSuites();
}

void AEGP_SuiteHandler::ReleaseSuite(const A_char *nameZ, A_long versionL)
{
	i_pica_basicP->ReleaseSuite(nameZ, versionL);
}
