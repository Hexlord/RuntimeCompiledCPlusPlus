//
// Copyright (c) 2010-2011 Matthew Jack and Doug Binks
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.

//
// Notes:
//   - We use a single intermediate directory for compiled .obj files, which means
//     we don't support compiling multiple files with the same name. Could fix this
//     with either mangling names to include paths,  or recreating folder structure
//
//

#include "Compiler.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <sstream>
#include <vector>
#include <set>
#include "FileSystemUtils.h"

#include "assert.h"
#include <process.h>

#include "ICompilerLogger.h"

using namespace std;
using namespace FileSystemUtils;

struct VSVersionInfo
{
    std::string		Path;
};

const std::string	c_CompletionToken( "_COMPLETION_TOKEN_" );

void GetPathsOfVisualStudioInstalls( std::vector<VSVersionInfo>* pVersions, ICompilerLogger * pLogger );

void ReadAndHandleOutputThread( LPVOID arg );

struct CmdProcess
{
    CmdProcess();
    ~CmdProcess();

    void InitialiseProcess();
    void WriteInput(std::string& input);
    void CleanupProcessAndPipes();


    PROCESS_INFORMATION m_CmdProcessInfo;
    HANDLE				m_CmdProcessOutputRead;
    HANDLE				m_CmdProcessInputWrite;
    volatile bool		m_bIsComplete;
    ICompilerLogger*    m_pLogger;
    bool				m_bStoreCmdOutput;
    std::string         m_CmdOutput;
};

class PlatformCompilerImplData
{
public:
    PlatformCompilerImplData();
    ~PlatformCompilerImplData();

    CmdProcess          m_CmdProcess;
    ICompilerLogger*	m_pLogger;
};

Compiler::Compiler()
        : m_pImplData( 0 )
        , m_bFastCompileMode( false )
{
}

Compiler::~Compiler()
{
    delete m_pImplData;
}

std::string Compiler::GetObjectFileExtension() const
{
    return ".obj";
}

bool Compiler::GetIsComplete() const
{
    bool bComplete = m_pImplData->m_CmdProcess.m_bIsComplete;
    if( bComplete & !m_bFastCompileMode )
    {
        m_pImplData->m_CmdProcess.CleanupProcessAndPipes();
    }
    return bComplete;
}

void Compiler::Initialise( ICompilerLogger * pLogger )
{
    m_pImplData = new PlatformCompilerImplData;
    m_pImplData->m_pLogger = pLogger;
    m_pImplData->m_CmdProcess.m_pLogger = pLogger;
}

void Compiler::RunCompile(	const std::vector<FileSystemUtils::Path>&	filesToCompile_,
                              const CompilerOptions&						compilerOptions_,
                              std::vector<FileSystemUtils::Path>			linkLibraryList_,
                              const FileSystemUtils::Path&				moduleName_ )
{
    m_pImplData->m_CmdProcess.m_bIsComplete = false;
    //optimization and c runtime
    std::string compileString = string{"g++"} + " " + "-g -fvisibility=hidden -shared ";

    RCppOptimizationLevel optimizationLevel = GetActualOptimizationLevel( compilerOptions_.optimizationLevel );
    switch( optimizationLevel )
    {
        case RCCPPOPTIMIZATIONLEVEL_DEFAULT:
            assert(false);
        case RCCPPOPTIMIZATIONLEVEL_DEBUG:
            compileString += "-O0 ";
            break;
        case RCCPPOPTIMIZATIONLEVEL_PERF:
            compileString += "-Os ";
            break;
        case RCCPPOPTIMIZATIONLEVEL_NOT_SET:;
        case RCCPPOPTIMIZATIONLEVEL_SIZE:;
    }

    if( NULL == m_pImplData->m_CmdProcess.m_CmdProcessInfo.hProcess )
    {
        m_pImplData->m_CmdProcess.InitialiseProcess();
    }

    compileString += compilerOptions_.compileOptions;
    compileString += " ";

    bool bHaveLinkOptions = ( 0 != compilerOptions_.linkOptions.length() );
    if(bHaveLinkOptions )
    {
        compileString += "-Wl,";
        compileString += compilerOptions_.linkOptions;
        compileString += " ";
    }

    if(compilerOptions_.libraryDirList.size() )
    {
        for( size_t i = 0; i < compilerOptions_.libraryDirList.size(); ++i )
        {
            compileString += "-L\"" + compilerOptions_.libraryDirList[i].m_string + "\" ";
            compileString += "-F\"" + compilerOptions_.libraryDirList[i].m_string + "\" ";
        }
    }

    // Check for intermediate directory, create it if required
    // There are a lot more checks and robustness that could be added here
    if ( !compilerOptions_.intermediatePath.Exists() )
    {
        bool success = compilerOptions_.intermediatePath.CreateDir();
        if( success && m_pImplData->m_pLogger ) { m_pImplData->m_pLogger->LogInfo("Created intermediate folder \"%s\"\n", compilerOptions_.intermediatePath.c_str()); }
        else if( m_pImplData->m_pLogger ) { m_pImplData->m_pLogger->LogError("Error creating intermediate folder \"%s\"\n", compilerOptions_.intermediatePath.c_str()); }

        // add save object files
        compileString = "cd \"" + compilerOptions_.intermediatePath.m_string + "\"\n" + compileString + " --save-temps ";

    }

    //create include path search string
    std::string strIncludeFiles;
    for( size_t i = 0; i < compilerOptions_.includeDirList.size(); ++i )
    {
        compileString += "-I\"" + compilerOptions_.includeDirList[i].m_string + "\" ";
    }

#ifdef UNICODE
    compileString += "-DUNICODE -D_UNICODE ";
#endif

    compileString += "-o \"" + moduleName_.m_string + "\" ";

    // When using multithreaded compilation, listing a file for compilation twice can cause errors, hence
    // we do a final filtering of input here.
    // See http://msdn.microsoft.com/en-us/library/bb385193.aspx - "Source Files and Build Order"

    // Create compile path search string
    std::set<std::string> filteredPaths;
    for( size_t i = 0; i < filesToCompile_.size(); ++i )
    {
        std::string strPath = filesToCompile_[i].m_string;
        FileSystemUtils::ToLowerInPlace(strPath);

        std::set<std::string>::const_iterator it = filteredPaths.find(strPath);
        if (it == filteredPaths.end())
        {
            compileString += "\"" + strPath + "\" ";
            filteredPaths.insert(strPath);
        }
    }

    for( size_t i = 0; i < linkLibraryList_.size(); ++i )
    {
        compileString += " \"" + linkLibraryList_[i].m_string + "\" ";
    }
    if( m_pImplData->m_pLogger )
        m_pImplData->m_pLogger->LogInfo( "%s", compileString.c_str() ); // use %s to prevent any tokens in compile string being interpreted as formating

    compileString += "\necho ";
    compileString += c_CompletionToken + "\n";

    m_pImplData->m_CmdProcess.WriteInput( compileString );
}

void ReadAndHandleOutputThread( LPVOID arg )
{
    CmdProcess* pCmdProc = (CmdProcess*)arg;

    CHAR lpBuffer[1024];
    DWORD nBytesRead;
    bool bReadActive = true;
    while( bReadActive )
    {
        if( !ReadFile( pCmdProc->m_CmdProcessOutputRead,lpBuffer,sizeof(lpBuffer)-1,
                       &nBytesRead,NULL) || !nBytesRead)
        {
            bReadActive = false;
            if( GetLastError() != ERROR_BROKEN_PIPE)	//broken pipe is OK
            {
                if(pCmdProc->m_pLogger ) pCmdProc->m_pLogger->LogError( "[RuntimeCompiler] Redirect of compile output failed on read\n" );
            }
        }
        else
        {
            // Add null termination
            lpBuffer[nBytesRead]=0;

            //fist check for completion token...
            std::string buffer( lpBuffer );
            size_t found = buffer.find( c_CompletionToken );
            if( found != std::string::npos )
            {
                //we've found the completion token, which means we quit
                buffer = buffer.substr( 0, found );
                if( !pCmdProc->m_bStoreCmdOutput && pCmdProc->m_pLogger ) pCmdProc->m_pLogger->LogInfo("[RuntimeCompiler] Complete\n");
                pCmdProc->m_bIsComplete = true;
            }
            if( bReadActive || buffer.length() ) //don't output blank last line
            {
                if( pCmdProc->m_bStoreCmdOutput )
                {
                    pCmdProc->m_CmdOutput += buffer;
                }
                else
                {
                    //check if this is an error
                    size_t errorFound = buffer.find( " : error " );
                    size_t fatalErrorFound = buffer.find( " : fatal error " );
                    if( ( errorFound != std::string::npos ) || ( fatalErrorFound != std::string::npos ) )
                    {
                        if(pCmdProc->m_pLogger ) pCmdProc->m_pLogger->LogError( "%s", buffer.c_str() );
                    }
                    else
                    {
                        if(pCmdProc->m_pLogger ) pCmdProc->m_pLogger->LogInfo( "%s", buffer.c_str() );
                    }
                }
            }
        }
    }
}

PlatformCompilerImplData::PlatformCompilerImplData() :
        m_pLogger(NULL)
{
}

PlatformCompilerImplData::~PlatformCompilerImplData()
{
}

CmdProcess::CmdProcess()
        : m_CmdProcessOutputRead(NULL)
        , m_CmdProcessInputWrite(NULL)
        , m_bIsComplete(false)
        , m_pLogger(NULL)
        , m_bStoreCmdOutput(false)
{
    ZeroMemory(&m_CmdProcessInfo, sizeof(m_CmdProcessInfo));
}

void CmdProcess::InitialiseProcess()
{
    wchar_t* pCommandLine = L"cmd /q /K @PROMPT $";

    //init compile process
    STARTUPINFOW				si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    // Set up the security attributes struct.
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;


    // Create the child output pipe.
    //redirection of output
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    HANDLE hOutputReadTmp = NULL, hOutputWrite = NULL, hErrorWrite = NULL;
    if (!CreatePipe(&hOutputReadTmp, &hOutputWrite, &sa, 20 * 1024))
    {
        if (m_pLogger) m_pLogger->LogError("[RuntimeCompiler] Failed to create output redirection pipe\n");
        goto ERROR_EXIT;
    }
    si.hStdOutput = hOutputWrite;

    // Create a duplicate of the output write handle for the std error
    // write handle. This is necessary in case the child application
    // closes one of its std output handles.
    if (!DuplicateHandle(GetCurrentProcess(), hOutputWrite,
                         GetCurrentProcess(), &hErrorWrite, 0,
                         TRUE, DUPLICATE_SAME_ACCESS))
    {
        if (m_pLogger) m_pLogger->LogError("[RuntimeCompiler] Failed to duplicate error output redirection pipe\n");
        goto ERROR_EXIT;
    }
    si.hStdError = hErrorWrite;


    // Create new output read handle and the input write handles. Set
    // the Properties to FALSE. Otherwise, the child inherits the
    // properties and, as a result, non-closeable handles to the pipes
    // are created.
    if (si.hStdOutput)
    {
        if (!DuplicateHandle(GetCurrentProcess(), hOutputReadTmp,
                             GetCurrentProcess(),
                             &m_CmdProcessOutputRead, // Address of new handle.
                             0, FALSE, // Make it uninheritable.
                             DUPLICATE_SAME_ACCESS))
        {
            if (m_pLogger) m_pLogger->LogError("[RuntimeCompiler] Failed to duplicate output read pipe\n");
            goto ERROR_EXIT;
        }
        CloseHandle(hOutputReadTmp);
        hOutputReadTmp = NULL;
    }


    HANDLE hInputRead, hInputWriteTmp;
    // Create a pipe for the child process's STDIN.
    if (!CreatePipe(&hInputRead, &hInputWriteTmp, &sa, 4096))
    {
        if (m_pLogger) m_pLogger->LogError("[RuntimeCompiler] Failed to create input pipes\n");
        goto ERROR_EXIT;
    }
    si.hStdInput = hInputRead;

    // Create new output read handle and the input write handles. Set
    // the Properties to FALSE. Otherwise, the child inherits the
    // properties and, as a result, non-closeable handles to the pipes
    // are created.
    if (si.hStdOutput)
    {
        if (!DuplicateHandle(GetCurrentProcess(), hInputWriteTmp,
                             GetCurrentProcess(),
                             &m_CmdProcessInputWrite, // Address of new handle.
                             0, FALSE, // Make it uninheritable.
                             DUPLICATE_SAME_ACCESS))
        {
            if (m_pLogger) m_pLogger->LogError("[RuntimeCompiler] Failed to duplicate input write pipe\n");
            goto ERROR_EXIT;
        }
    }
    /*
    // Ensure the write handle to the pipe for STDIN is not inherited.
    if ( !SetHandleInformation(hInputWrite, HANDLE_FLAG_INHERIT, 0) )
    {
    m_pLogger->LogError("[RuntimeCompiler] Failed to make input write pipe non inheritable\n");
    goto ERROR_EXIT;
    }
    */

    //CreateProcessW won't accept a const pointer, so copy to an array
    wchar_t pCmdLineNonConst[1024];
    wcscpy_s(pCmdLineNonConst, pCommandLine);
    CreateProcessW(
            NULL,				//__in_opt     LPCTSTR lpApplicationName,
            pCmdLineNonConst,			//__inout_opt  LPTSTR lpCommandLine,
            NULL,				//__in_opt     LPSECURITY_ATTRIBUTES lpProcessAttributes,
            NULL,				//__in_opt     LPSECURITY_ATTRIBUTES lpThreadAttributes,
            TRUE,				//__in         BOOL bInheritHandles,
            0,				//__in         DWORD dwCreationFlags,
            NULL,				//__in_opt     LPVOID lpEnvironment,
            NULL,				//__in_opt     LPCTSTR lpCurrentDirectory,
            &si,				//__in         LPSTARTUPINFO lpStartupInfo,
            &m_CmdProcessInfo				//__out        LPPROCESS_INFORMATION lpProcessInformation
    );

    //launch threaded read.
    _beginthread(ReadAndHandleOutputThread, 0, this); //this will exit when process for compile is closed


    ERROR_EXIT:
    if( hOutputReadTmp )
    {
        CloseHandle( hOutputReadTmp );
    }
    if( hOutputWrite )
    {
        CloseHandle(hOutputWrite);
    }
    if( hErrorWrite )
    {
        CloseHandle( hErrorWrite );
    }
}


void CmdProcess::WriteInput( std::string& input )
{
    DWORD nBytesWritten;
    DWORD length = (DWORD)input.length();
    WriteFile( m_CmdProcessInputWrite , input.c_str(), length, &nBytesWritten, NULL);
}

void CmdProcess::CleanupProcessAndPipes()
{
    // do not reset m_bIsComplete and other members here, just process and pipes
    if( m_CmdProcessInfo.hProcess )
    {
        TerminateProcess(m_CmdProcessInfo.hProcess, 0);
        TerminateThread(m_CmdProcessInfo.hThread, 0);
        CloseHandle(m_CmdProcessInfo.hThread);
        ZeroMemory(&m_CmdProcessInfo, sizeof(m_CmdProcessInfo));
        CloseHandle(m_CmdProcessInputWrite);
        m_CmdProcessInputWrite = 0;
        CloseHandle(m_CmdProcessOutputRead);
        m_CmdProcessOutputRead = 0;
    }
}

CmdProcess::~CmdProcess()
{
    CleanupProcessAndPipes();
}
