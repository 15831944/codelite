#if HAS_LIBCLANG

#include "clang_driver.h"
#include "clang_code_completion.h"
#include "compilation_database.h"
#include "pluginmanager.h"
#include "clang_macro_handler.h"
#include <wx/regex.h>
#include "code_completion_box.h"
#include "clangpch_cache.h"
#include "asyncprocess.h"
#include "frame.h"
#include "processreaderthread.h"
#include "navigationmanager.h"
#include "compiler_command_line_parser.h"
#include "file_logger.h"
#include "pluginmanager.h"
#include "macromanager.h"
#include "includepathlocator.h"
#include "frame.h"
#include "macromanager.h"
#include <memory>
#include "environmentconfig.h"
#include "tags_options_data.h"
#include "ctags_manager.h"
#include <wx/tokenzr.h>
#include "processreaderthread.h"
#include "manager.h"
#include "project.h"
#include "configuration_mapping.h"
#include "procutils.h"
#include "localworkspace.h"
#include "fileextmanager.h"
#include "globals.h"
#include <set>
#include "event_notifier.h"
#include "plugin.h"
#include "browse_record.h"
#include "mainbook.h"

static bool wxIsWhitespace(wxChar ch)
{
    return ch == wxT(' ') || ch == wxT('\t') || ch == wxT('\r') || ch == wxT('\n');
}

#define cstr(x) x.mb_str(wxConvUTF8).data()
#define CHECK_CLANG_ENABLED() \
    if(!(TagsManagerST::Get()->GetCtagsOptions().GetClangOptions() & CC_CLANG_ENABLED))\
        return;\
     
ClangDriver::ClangDriver()
    : m_isBusy(false)
    , m_activeEditor(NULL)
    , m_position(wxNOT_FOUND)
{
    m_index = clang_createIndex(0, 0);
    m_pchMakerThread.SetSleepInterval(30);
    m_pchMakerThread.Start();
    EventNotifier::Get()->Connect(wxEVT_CLANG_PCH_CACHE_ENDED,   wxCommandEventHandler(ClangDriver::OnPrepareTUEnded), NULL, this);
    EventNotifier::Get()->Connect(wxEVT_CLANG_PCH_CACHE_CLEARED, wxCommandEventHandler(ClangDriver::OnCacheCleared),   NULL, this);
    EventNotifier::Get()->Connect(wxEVT_CLANG_TU_CREATE_ERROR,   wxCommandEventHandler(ClangDriver::OnTUCreateError),  NULL, this);
    EventNotifier::Get()->Connect(wxEVT_CMD_CLANG_MACRO_HADNLER_DELETE, wxCommandEventHandler(ClangDriver::OnDeletMacroHandler),  NULL, this);
    EventNotifier::Get()->Connect(wxEVT_WORKSPACE_LOADED, wxCommandEventHandler(ClangDriver::OnWorkspaceLoaded), NULL, this);
}

ClangDriver::~ClangDriver()
{
    // Disconnect all events before we perform anything elase
    EventNotifier::Get()->Disconnect(wxEVT_CLANG_PCH_CACHE_ENDED, wxCommandEventHandler(ClangDriver::OnPrepareTUEnded),  NULL, this);
    EventNotifier::Get()->Disconnect(wxEVT_CLANG_PCH_CACHE_CLEARED, wxCommandEventHandler(ClangDriver::OnCacheCleared),  NULL, this);
    EventNotifier::Get()->Disconnect(wxEVT_CLANG_TU_CREATE_ERROR,   wxCommandEventHandler(ClangDriver::OnTUCreateError), NULL, this);
    EventNotifier::Get()->Disconnect(wxEVT_CMD_CLANG_MACRO_HADNLER_DELETE, wxCommandEventHandler(ClangDriver::OnDeletMacroHandler),  NULL, this);
    EventNotifier::Get()->Disconnect(wxEVT_WORKSPACE_LOADED, wxCommandEventHandler(ClangDriver::OnWorkspaceLoaded), NULL, this);

    m_pchMakerThread.Stop();
    m_pchMakerThread.ClearCache(); // clear cache and dispose all translation units
    clang_disposeIndex(m_index);
}

ClangThreadRequest* ClangDriver::DoMakeClangThreadRequest(IEditor* editor, WorkingContext context)
{
    /////////////////////////////////////////////////////////////////
    // Prepare all the buffers required by the thread
    wxString fileName = editor->GetFileName().GetFullPath();

    wxString currentBuffer = editor->GetTextRange(0, editor->GetLength());
    wxString filterWord;

    // Move backward until we found our -> or :: or .
    m_position = editor->GetCurrentPosition();
    wxString tmpBuffer = editor->GetTextRange(0, editor->GetCurrentPosition());
    while ( !tmpBuffer.IsEmpty() ) {

        // Context word complete and we found a whitespace - break the search
        if((context == CTX_WordCompletion || context == CTX_Calltip) && wxIsWhitespace(tmpBuffer.Last())) {
            break;
        }

        if(context == CTX_WordCompletion && !IsValidCppIndetifier(tmpBuffer.Last())) {
            break;
        }

        if(tmpBuffer.EndsWith(wxT("->")) || tmpBuffer.EndsWith(wxT(".")) || tmpBuffer.EndsWith(wxT("::"))) {
            break;

        } else {
            filterWord.Prepend(tmpBuffer.Last());
            tmpBuffer.RemoveLast();
        }
    }

    // Get the current line's starting pos
    int lineStartPos = editor->PosFromLine( editor->GetCurrentLine() );
    int column       = editor->GetCurrentPosition() - lineStartPos  + 1;
    int lineNumber   = editor->GetCurrentLine() + 1;

    if(context == CTX_GotoDecl || context == CTX_GotoImpl) {
        wxString sel = editor->GetSelection();
        if(sel.IsEmpty()) {
            filterWord = editor->GetWordAtCaret();

        } else {
            filterWord = sel;
            column = editor->GetSelectionStart() - lineStartPos + 1;
        }

    } else {
        column -= (int) filterWord.Length();
    }

    // Column can not be lower than 1
    switch(context) {
    case CTX_Calltip:
    case CTX_WordCompletion:
    case CTX_CodeCompletion:
        if(column < 1) {
            CL_DEBUG(wxT("Clang: column can not be lower than 1"));
            m_isBusy = false;
            return NULL;
        }
        break;
    default:
        break;
    }

    wxString projectPath;
    wxString pchFile;
    FileTypeCmpArgs_t compileFlags = DoPrepareCompilationArgs(editor->GetProjectName(), fileName, projectPath, pchFile);
    ClangThreadRequest* request = new ClangThreadRequest(m_index,
            fileName,
            currentBuffer,
            compileFlags,
            filterWord,
            context,
            lineNumber,
            column, DoCreateListOfModifiedBuffers(editor));
    request->SetPchFile(pchFile);
    return request;
}

void ClangDriver::CodeCompletion(IEditor* editor)
{
    if(m_isBusy) {
        if(editor) {
            CodeCompletionBox::Get().CancelTip();
            CodeCompletionBox::Get().ShowTip( wxT("<b>clang: </b>Code Completion Message:<hr>A lengthy operation is in progress..."), dynamic_cast<LEditor*>(m_activeEditor));
        }
        return;
    }

    CL_DEBUG(wxT(" ==========> ClangDriver::CodeCompletion() started <=============="));

    if(!editor) {
        CL_WARNING(wxT("ClangDriver::CodeCompletion() called with NULL editor!"));
        return;
    }

    m_activeEditor = editor;
    m_isBusy       = true;
    ClangThreadRequest * request = DoMakeClangThreadRequest(m_activeEditor, GetContext());

    // Failed to prepare request?
    if(request == NULL) {
        m_isBusy      = false;
        m_activeEditor = NULL;
        return;
    }

    /////////////////////////////////////////////////////////////////
    // Put a request on the parsing thread
    //
    m_pchMakerThread.Add( request );
}

void ClangDriver::Abort()
{
    DoCleanup();
}

FileTypeCmpArgs_t ClangDriver::DoPrepareCompilationArgs(const wxString& projectName, const wxString& sourceFile, wxString& projectPath, wxString& pchfile)
{
    FileTypeCmpArgs_t cmpArgs;

    cmpArgs.insert(std::make_pair(FileExtManager::TypeSourceC,   wxArrayString()));
    cmpArgs.insert(std::make_pair(FileExtManager::TypeSourceCpp, wxArrayString()));

    wxArrayString args;
    wxString      errMsg;

    wxArrayString &cppCompileArgs = cmpArgs[FileExtManager::TypeSourceCpp];
    wxArrayString &cCompileArgs   = cmpArgs[FileExtManager::TypeSourceC];

    // Build the TU file name
    wxFileName fnSourceFile(sourceFile);
    pchfile << WorkspaceST::Get()->GetWorkspaceFileName().GetPath() << wxFileName::GetPathSeparator() << wxT(".clang");

    {
        wxLogNull nl;
        wxMkdir(pchfile);
    }

    pchfile << wxFileName::GetPathSeparator() << fnSourceFile.GetFullName() << wxT(".TU");


    CompilationDatabase cdb;
    static bool once = false;
    if ( !wxFileName::FileExists( cdb.GetFileName().GetFullPath() ) && !once ) {
        once = true;

        wxString msg;
        msg << _("Could not locate compilation database: ")
            << cdb.GetFileName().GetFullPath() << wxT("\n\n")
            << _("This file should be created automatically for you.\nIf you don't have it, please run a full rebuild of your workspace\n\n")
            << _("If this is a custom build project (i.e. project that uses a custom makefile),\nplease set the CXX and CC environment variables like this:\n")
            << _("CXX=codelitegcc g++\n")
            << _("CC=codelitegcc gcc\n\n");

        clMainFrame::Get()->GetMainBook()->ShowMessage( msg,
                true,
                PluginManager::Get()->GetStdIcons()->LoadBitmap(wxT("messages/48/tip")),
                ButtonDetails(),
                ButtonDetails(),
                ButtonDetails(),
                CheckboxDetails(wxT("CodeCompletionMissingCompilationDB")));

    } else {
        cdb.Open();
        if( cdb.IsOpened() ) {
            CL_DEBUG(wxT("Loading compilation flags for file: %s"), fnSourceFile.GetFullPath().c_str());
            wxString compilationLine, cwd;
            cdb.CompilationLine(fnSourceFile.GetFullPath(), compilationLine, cwd);
            cdb.Close();

            CompilerCommandLineParser cclp(compilationLine);
            cclp.MakeAbsolute(cwd);

            CL_DEBUG(wxT("Loaded compilation flags: %s"), compilationLine.c_str());
            args.insert(args.end(), cclp.GetIncludesWithPrefix().begin(), cclp.GetIncludesWithPrefix().end());
            args.insert(args.end(), cclp.GetMacrosWithPrefix().begin(),   cclp.GetMacrosWithPrefix().end());
            args.Add(cclp.GetStandardWithPrefix());

        }
    }

    const TagsOptionsData& options = TagsManagerST::Get()->GetCtagsOptions();

    ///////////////////////////////////////////////////////////////////////
    // add global clang include paths
    wxString strGlobalIncludes = options.GetClangSearchPaths();
    wxArrayString globalIncludes = wxStringTokenize(strGlobalIncludes, wxT("\n\r"), wxTOKEN_STRTOK);
    for(size_t i=0; i<globalIncludes.GetCount(); i++) {
        wxFileName fn(globalIncludes.Item(i).Trim().Trim(false), wxT(""));
        fn.MakeAbsolute(projectPath);

        cppCompileArgs.Add(wxString::Format(wxT("-I%s"), fn.GetPath().c_str()));
        cCompileArgs.Add(wxString::Format(wxT("-I%s"), fn.GetPath().c_str()));
    }

    ///////////////////////////////////////////////////////////////////////
    // Workspace setting additional flags
    ///////////////////////////////////////////////////////////////////////

    // Include paths
    wxArrayString workspaceIncls, dummy;
    LocalWorkspaceST::Get()->GetParserPaths(workspaceIncls, dummy);
    for(size_t i=0; i<workspaceIncls.GetCount(); i++) {
        wxFileName fn(workspaceIncls.Item(i).Trim().Trim(false), wxT(""));
        fn.MakeAbsolute(WorkspaceST::Get()->GetWorkspaceFileName().GetPath());
        cppCompileArgs.Add(wxString::Format(wxT("-I%s"), fn.GetPath().c_str()));
        cCompileArgs.Add(wxString::Format(wxT("-I%s"), fn.GetPath().c_str()));
    }

    // Macros
    wxString strWorkspaceMacros;
    LocalWorkspaceST::Get()->GetParserMacros(strWorkspaceMacros);
    wxArrayString workspaceMacros = wxStringTokenize(strWorkspaceMacros, wxT("\n\r"), wxTOKEN_STRTOK);
    for(size_t i=0; i<workspaceMacros.GetCount(); i++) {
        cppCompileArgs.Add(wxString::Format(wxT("-D%s"), workspaceMacros.Item(i).Trim().Trim(false).c_str()));
        cCompileArgs.Add(wxString::Format(wxT("-D%s"), workspaceMacros.Item(i).Trim().Trim(false).c_str()));
    }

    cppCompileArgs.insert(cppCompileArgs.end(), args.begin(), args.end());
    cCompileArgs.insert(cCompileArgs.end(), args.begin(), args.end());

    // Remove some of the flags which are known to cause problems to clang
    int where = wxNOT_FOUND;

    where = cppCompileArgs.Index(wxT("-fno-strict-aliasing"));
    if(where != wxNOT_FOUND) cppCompileArgs.RemoveAt(where);

    where = cppCompileArgs.Index(wxT("-mthreads"));
    if(where != wxNOT_FOUND) cppCompileArgs.RemoveAt(where);

    where = cppCompileArgs.Index(wxT("-pipe"));
    if(where != wxNOT_FOUND) cppCompileArgs.RemoveAt(where);

    where = cppCompileArgs.Index(wxT("-fmessage-length=0"));
    if(where != wxNOT_FOUND) cppCompileArgs.RemoveAt(where);

    where = cppCompileArgs.Index(wxT("-fPIC"));
    if(where != wxNOT_FOUND) cppCompileArgs.RemoveAt(where);

    // Now do the same for the "C" arguments
    where = cCompileArgs.Index(wxT("-fno-strict-aliasing"));
    if(where != wxNOT_FOUND) cCompileArgs.RemoveAt(where);

    where = cCompileArgs.Index(wxT("-mthreads"));
    if(where != wxNOT_FOUND) cCompileArgs.RemoveAt(where);

    where = cCompileArgs.Index(wxT("-pipe"));
    if(where != wxNOT_FOUND) cCompileArgs.RemoveAt(where);

    where = cCompileArgs.Index(wxT("-fmessage-length=0"));
    if(where != wxNOT_FOUND) cCompileArgs.RemoveAt(where);

    where = cCompileArgs.Index(wxT("-fPIC"));
    if(where != wxNOT_FOUND) cCompileArgs.RemoveAt(where);

    return cmpArgs;
}

wxArrayString ClangDriver::DoExpandBacktick(const wxString& backtick, const wxString &projectName)
{
    wxString tmp;
    wxString cmpOption = backtick;
    // Expand backticks / $(shell ...) syntax supported by codelite
    if(cmpOption.StartsWith(wxT("$(shell "), &tmp) || cmpOption.StartsWith(wxT("`"), &tmp)) {
        cmpOption = tmp;
        tmp.Clear();
        if(cmpOption.EndsWith(wxT(")"), &tmp) || cmpOption.EndsWith(wxT("`"), &tmp)) {
            cmpOption = tmp;
        }

        if(m_backticks.find(cmpOption) == m_backticks.end()) {

            CL_DEBUG(wxT("DoExpandBacktick(): executing: '%s'"), cmpOption.c_str());
            // Expand the backticks into their value
            wxString expandedValue = wxShellExec(cmpOption, projectName);
            m_backticks[cmpOption] = expandedValue;
            cmpOption = expandedValue;
            CL_DEBUG(wxT("DoExpandBacktick(): result: '%s'"), expandedValue.c_str());

        } else {
            cmpOption = m_backticks.find(cmpOption)->second;
        }
    }

    CompilerCommandLineParser p(cmpOption);
    wxArrayString opts;
    opts.insert(opts.end(), p.GetIncludesWithPrefix().begin(), p.GetIncludesWithPrefix().end());
    opts.insert(opts.end(), p.GetMacrosWithPrefix().begin(),   p.GetMacrosWithPrefix().end());

    if( p.GetStandardWithPrefix().IsEmpty() == false )
        opts.Add(p.GetStandardWithPrefix());
    return opts;
}

void ClangDriver::ClearCache()
{
    m_pchMakerThread.ClearCache();
    m_backticks.clear();
}

bool ClangDriver::IsCacheEmpty()
{
    return m_pchMakerThread.IsCacheEmpty();
}

void ClangDriver::DoCleanup()
{
    m_isBusy = false;
    m_activeEditor = NULL;
}

void ClangDriver::DoParseCompletionString(CXCompletionString str, int depth, wxString &entryName, wxString &signature, wxString &completeString, wxString &returnValue)
{

    bool collectingSignature = false;
    int numOfChunks = clang_getNumCompletionChunks(str);
    for (int j=0 ; j<numOfChunks; j++) {

        CXString chunkText = clang_getCompletionChunkText(str, j);
        CXCompletionChunkKind chunkKind = clang_getCompletionChunkKind(str, j);

        switch(chunkKind) {
        case CXCompletionChunk_TypedText:
            entryName = wxString(clang_getCString(chunkText), wxConvUTF8);
            completeString += entryName;
            break;

        case CXCompletionChunk_ResultType:
            completeString += wxString(clang_getCString(chunkText), wxConvUTF8);
            completeString += wxT(" ");
            returnValue = wxString(clang_getCString(chunkText), wxConvUTF8);
            break;

        case CXCompletionChunk_Optional: {
            // Optional argument
            CXCompletionString optStr = clang_getCompletionChunkCompletionString(str, j);
            wxString optionalString;
            wxString dummy;
            // Once we hit the 'Optional Chunk' only the 'completeString' is matter
            DoParseCompletionString(optStr, depth + 1, dummy, dummy, optionalString, dummy);
            if(collectingSignature) {
                signature += optionalString;
            }
            completeString += optionalString;
        }
        break;
        case CXCompletionChunk_LeftParen:
            collectingSignature = true;
            signature += wxT("(");
            completeString += wxT("(");
            break;

        case CXCompletionChunk_RightParen:
            collectingSignature = true;
            signature += wxT(")");
            completeString += wxT(")");
            break;

        default:
            if(collectingSignature) {
                signature += wxString(clang_getCString(chunkText), wxConvUTF8);
            }
            completeString += wxString(clang_getCString(chunkText), wxConvUTF8);
            break;
        }
        clang_disposeString(chunkText);
    }

    // To make this tag compatible with ctags one, we need to place
    // a /^ and $/ in the pattern string (we add this only to the top level completionString)
    if(depth == 0) {
        completeString.Prepend(wxT("/^ "));
        completeString.Append(wxT(" $/"));
    }
}

void ClangDriver::OnPrepareTUEnded(wxCommandEvent& e)
{
    // Our thread is done
    m_isBusy = false;

    // Sanity
    ClangThreadReply* reply = (ClangThreadReply*) e.GetClientData();
    if(!reply)
        return;

    // Make sure we delete the reply at the end...
    std::auto_ptr<ClangThreadReply> ap(reply);

    if(reply->context == ::CTX_CachePCH || reply->context == ::CTX_ReparseTU) {
        return; // Nothing more to be done
    }

    if(reply->context == CTX_GotoDecl || reply->context == CTX_GotoImpl) {
        // Unlike other context's the 'filename' specified here
        // does not belong to an editor (it could, but it is not necessarily true)
        DoGotoDefinition(reply);
        return;
    }

    // Adjust the activeEditor to fit the filename
    IEditor *editor = clMainFrame::Get()->GetMainBook()->FindEditor(reply->filename);
    if(!editor) {
        CL_DEBUG(wxT("Could not find an editor for file %s"), reply->filename.c_str());
        return;
    }

    m_activeEditor = editor;

    // What should we do with the TU?
    switch(reply->context) {
    case CTX_CachePCH:
        // Nothing more to be done
        return;
    default:
        break;
    }

    if(!reply->results) {
        // Display an error message if needed, but not if the code-completion box
        // is visible
        if(reply->errorMessage.IsEmpty() == false && !m_activeEditor->IsCompletionBoxShown()) {
            CodeCompletionBox::Get().CancelTip();
            CodeCompletionBox::Get().ShowTip(reply->errorMessage, dynamic_cast<LEditor*>(m_activeEditor));
        }
        return;
    }

    if(m_activeEditor->GetCurrentPosition() < m_position) {
        CL_DEBUG(wxT("Current position is lower than the starting position, ignoring completion"));
        clang_disposeCodeCompleteResults(reply->results);
        return;
    }

    wxString typedString;
    if(m_activeEditor->GetCurrentPosition() > m_position) {
        // User kept on typing while the completion thread was working
        typedString = m_activeEditor->GetTextRange(m_position, m_activeEditor->GetCurrentPosition());
        if(typedString.find_first_not_of(wxT("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")) != wxString::npos) {
            // User typed some non valid identifier char, cancel code completion
            CL_DEBUG(wxT("User typed: %s since the completion thread started working until it ended, ignoring completion"), typedString.c_str());
            clang_disposeCodeCompleteResults(reply->results);
            return;
        }
    }

    // update the filter word
    reply->filterWord.Append(typedString);
    CL_DEBUG(wxT("clang completion: filter word is %s"), reply->filterWord.c_str());

    // For the Calltip, remove the opening brace from the filter string
    wxString filterWord = reply->filterWord;
    if(GetContext() == CTX_Calltip && filterWord.EndsWith(wxT("(")))
        filterWord.RemoveLast();

    wxString lowerCaseFilter = filterWord;
    lowerCaseFilter.MakeLower();

    unsigned numResults = reply->results->NumResults;
    clang_sortCodeCompletionResults(reply->results->Results, reply->results->NumResults);
    std::vector<TagEntryPtr> tags;
    for(unsigned i=0; i<numResults; i++) {
        CXCompletionResult result = reply->results->Results[i];
        CXCompletionString str    = result.CompletionString;
        CXCursorKind       kind   = result.CursorKind;

        if(kind == CXCursor_NotImplemented)
            continue;

        wxString entryName, entrySignature, entryPattern, entryReturnValue;
        DoParseCompletionString(str, 0, entryName, entrySignature, entryPattern, entryReturnValue);

        wxString lowerCaseName = entryName;
        lowerCaseName.MakeLower();

        if(!lowerCaseFilter.IsEmpty() && !lowerCaseName.StartsWith(lowerCaseFilter))
            continue;

        if (clang_getCompletionAvailability(str) != CXAvailability_Available)
            continue;

        TagEntry *t = new TagEntry();
        TagEntryPtr tag(t);
        tag->SetIsClangTag(true);
        tag->SetName      (entryName);
        tag->SetPattern   (entryPattern);
        tag->SetSignature (entrySignature);

        // Add support for clang comment parsing
        CXString BriefComment = clang_getCompletionBriefComment(str);
        const char* comment = clang_getCString(BriefComment);
        if( comment && comment[0] != '\0' ) {
            tag->SetComment(wxString(comment, wxConvUTF8));
        }

        clang_disposeString(BriefComment);

        switch(kind) {
        case CXCursor_EnumConstantDecl:
            tag->SetKind(wxT("enumerator"));
            break;

        case CXCursor_EnumDecl:
            tag->SetKind(wxT("enum"));
            break;

        case CXCursor_CXXMethod:
        case CXCursor_Constructor:
        case CXCursor_Destructor:
        case CXCursor_FunctionDecl:
        case CXCursor_FunctionTemplate:
            tag->SetKind(wxT("prototype"));
            break;

        case CXCursor_MacroDefinition:
            tag->SetKind(wxT("macro"));
            break;

        case CXCursor_Namespace:
            tag->SetKind(wxT("namespace"));
            break;

        case CXCursor_ClassDecl:
        case CXCursor_ClassTemplate:
        case CXCursor_ClassTemplatePartialSpecialization:
            tag->SetKind(wxT("class"));
            break;

        case CXCursor_StructDecl:
            tag->SetKind(wxT("struct"));
            break;

        case CXCursor_TypeRef:
        case CXCursor_TypedefDecl:
            tag->SetKind(wxT("typedef"));
            tag->SetKind(wxT("typedef"));
            break;
        default:
            tag->SetKind(wxT("variable"));
            break;
        }

        tags.push_back(tag);
    }

    clang_disposeCodeCompleteResults(reply->results);

    CL_DEBUG(wxT("Building completion results... done "));
    if(GetContext() == CTX_Calltip) {
        std::vector<TagEntryPtr> tips;
        TagsManagerST::Get()->GetFunctionTipFromTags(tags, filterWord, tips);
        m_activeEditor->ShowCalltip(new clCallTip(tips));

    } else {
        m_activeEditor->ShowCompletionBox(tags, filterWord, true, NULL);

    }
}

void ClangDriver::QueueRequest(IEditor *editor, WorkingContext context)
{
    if(!editor)
        return;

    switch(context) {
    case CTX_CachePCH:
        break;
    default:
        CL_DEBUG(wxT("Context %d id not allowed to be queued"), (int)context);
        return;
    }

    m_pchMakerThread.Add( DoMakeClangThreadRequest(editor, context) );
}

void ClangDriver::ReparseFile(const wxString& filename)
{
    //CHECK_CLANG_ENABLED();
    //
    //ClangThreadRequest *req = new ClangThreadRequest(m_index, filename, wxT(""), wxArrayString(), wxT(""), ::CTX_ReparseTU, 0, 0);
    //m_pchMakerThread.Add( req );
    //CL_DEBUG(wxT("Queued request to re-parse file: %s"), filename.c_str());
}

void ClangDriver::OnCacheCleared(wxCommandEvent& e)
{
    e.Skip();

    CHECK_CLANG_ENABLED();

    // Reparse the current file
    IEditor *editor = clMainFrame::Get()->GetMainBook()->GetActiveEditor();
    if(editor) {
        wxString outputProjectPath, pchFile;
        ClangThreadRequest *req = new ClangThreadRequest(m_index,
                editor->GetFileName().GetFullPath(),
                editor->GetEditorText(),
                DoPrepareCompilationArgs(editor->GetProjectName(), editor->GetFileName().GetFullPath(), outputProjectPath, pchFile),
                wxT(""),
                ::CTX_CachePCH, 0, 0, DoCreateListOfModifiedBuffers(editor));

        req->SetPchFile(pchFile);
        m_pchMakerThread.Add( req );
        CL_DEBUG(wxT("OnCacheCleared:: Queued request to build TU for file: %s"), editor->GetFileName().GetFullPath().c_str());
    }
}

void ClangDriver::DoGotoDefinition(ClangThreadReply* reply)
{
    CHECK_CLANG_ENABLED();
    LEditor *editor = clMainFrame::Get()->GetMainBook()->OpenFile(reply->filename, wxEmptyString, reply->line);
    if(editor) {
        int pos = editor->PositionFromLine(reply->line - 1);
        editor->FindAndSelect(reply->filterWord,
                              reply->filterWord,
                              pos,
                              NULL);
    }
}

void ClangDriver::OnTUCreateError(wxCommandEvent& e)
{
    e.Skip();
    DoCleanup();
}

void ClangDriver::GetMacros(IEditor *editor)
{
    if( !editor )
        return;

    if(editor->GetProjectName().IsEmpty()) {
        // This file is not part of the workspace
        // do not attemp to color its preprocessors
        editor->GetSTC()->SetProperty(wxT("lexer.cpp.track.preprocessor"),  wxT("0"));
        editor->GetSTC()->SetProperty(wxT("lexer.cpp.update.preprocessor"), wxT("0"));
        editor->GetSTC()->SetKeyWords(4, wxT(""));
        editor->GetSTC()->Colourise(0, wxSTC_INVALID_POSITION);
        return;
    }

    wxString projectPath;
    wxString pchFile;
    FileTypeCmpArgs_t compileFlags = DoPrepareCompilationArgs(editor->GetProjectName(), editor->GetFileName().GetFullPath(), projectPath, pchFile);

    wxString cmd;
#ifdef __WXMAC__
    wxFileName exePath(wxStandardPaths::Get().GetDataDir(), wxT(""));
#else
    wxFileName exePath(wxStandardPaths::Get().GetExecutablePath());
#endif
    exePath.SetFullName(wxT("codelite-clang"));

#ifdef __WXMSW__
    exePath.SetExt(wxT("exe"));
#endif

    wxFileName outputFolder(pchFile);
    // Select the compilation args
    FileExtManager::FileType type = FileExtManager::TypeSourceCpp; // Default is C++

    switch(FileExtManager::GetType(editor->GetFileName().GetFullName())) {
    case FileExtManager::TypeSourceC:
        type = FileExtManager::TypeSourceC;
        break;
    default:
        // Use the default
        break;
    }

    const wxArrayString& compilerSwitches = compileFlags[type];
    cmd << wxT("\"") << exePath.GetFullPath() << wxT("\" parse-macros \"") << editor->GetFileName().GetFullPath() << wxT("\" \"") << outputFolder.GetPath() << wxT("\" ");
    for(size_t i=0; i<compilerSwitches.GetCount(); i++) {
        cmd << compilerSwitches.Item(i) << wxT(" ");
    }

    ClangMacroHandler *handler = new ClangMacroHandler();

    CL_DEBUG(wxT("Executing command: %s"), cmd.c_str());
    handler->SetProcessAndEditor( ::CreateAsyncProcess(handler, cmd), editor );

    if(handler->GetProcess() == NULL) {
        delete handler;
        return;
    }
}

void ClangDriver::OnDeletMacroHandler(wxCommandEvent& e)
{
    ClangMacroHandler *h = reinterpret_cast<ClangMacroHandler*>(e.GetClientData());
    if( h ) {
        delete h;
    }
}

void ClangDriver::OnWorkspaceLoaded(wxCommandEvent& event)
{
    event.Skip();

    wxLogNull nolog;
    wxString cachePath;
    cachePath << WorkspaceST::Get()->GetWorkspaceFileName().GetPath() << wxFileName::GetPathSeparator() << wxT(".clang");
    wxMkdir(cachePath);
    ClangTUCache::DeleteDirectoryContent(cachePath);
}

ClangThreadRequest::List_t ClangDriver::DoCreateListOfModifiedBuffers(IEditor* excludeEditor)
{
    // Collect all modified buffers and pass them to clang as well
    ClangThreadRequest::List_t modifiedBuffers;
    std::vector<LEditor*> editors;
    clMainFrame::Get()->GetMainBook()->GetAllEditors(editors);
    for(size_t i=0; i<editors.size(); i++) {

        if( editors.at(i) == excludeEditor || !editors.at(i)->IsModified())
            continue;

        modifiedBuffers.push_back( std::make_pair( editors.at(i)->GetFileName().GetFullPath(), editors.at(i)->GetText() ) );
    }
    return modifiedBuffers;
}

#endif // HAS_LIBCLANG
