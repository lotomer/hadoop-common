/**
* Licensed to the Apache Software Foundation (ASF) under one or more
* contributor license agreements. See the NOTICE file distributed with this
* work for additional information regarding copyright ownership. The ASF
* licenses this file to you under the Apache License, Version 2.0 (the
* "License"); you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
* 
* http://www.apache.org/licenses/LICENSE-2.0
* 
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
* WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
* License for the specific language governing permissions and limitations under
* the License.
*/

#include "common.h"
#include <errno.h>

enum CHMOD_WHO
{
  CHMOD_WHO_NONE  =    0,
  CHMOD_WHO_OTHER =   07,
  CHMOD_WHO_GROUP =  070,
  CHMOD_WHO_USER  = 0700,
  CHMOD_WHO_ALL   = CHMOD_WHO_OTHER | CHMOD_WHO_GROUP | CHMOD_WHO_USER
};

enum CHMOD_OP
{
  CHMOD_OP_INVALID,
  CHMOD_OP_PLUS,
  CHMOD_OP_MINUS,
  CHMOD_OP_EQUAL,
};

enum CHMOD_PERM
{
  CHMOD_PERM_NA =  00,
  CHMOD_PERM_R  =  01,
  CHMOD_PERM_W  =  02,
  CHMOD_PERM_X  =  04,
  CHMOD_PERM_LX = 010,
};

/*
 * We use the following struct to build a linked list of mode change actions.
 * The mode is described by the following grammar:
 *  mode         ::= clause [, clause ...]
 *  clause       ::= [who ...] [action ...]
 *  action       ::= op [perm ...] | op [ref]
 *  who          ::= a | u | g | o
 *  op           ::= + | - | =
 *  perm         ::= r | w | x | X
 *  ref          ::= u | g | o
 */
typedef struct _MODE_CHANGE_ACTION
{
  USHORT who;
  USHORT op;
  USHORT perm;
  USHORT ref;
  struct _MODE_CHANGE_ACTION *next_action;
} MODE_CHANGE_ACTION, *PMODE_CHANGE_ACTION;


const MODE_CHANGE_ACTION INIT_MODE_CHANGE_ACTION = {
  CHMOD_WHO_NONE, CHMOD_OP_INVALID, CHMOD_PERM_NA, CHMOD_WHO_NONE, NULL
};


static BOOL ParseOctalMode(LPCWSTR tsMask, USHORT *uMask);

static BOOL ParseMode(LPCWSTR modeString, PMODE_CHANGE_ACTION *actions);

static BOOL FreeActions(PMODE_CHANGE_ACTION actions);

static BOOL GetWindowsDACLs(__in USHORT unixMask, __in PSID pOwnerSid,
  __in PSID pGroupSid, __out PACL *ppNewDACL);

static BOOL ParseCommandLineArguments(__in int argc, __in wchar_t *argv[],
  __out BOOL *rec, __out_opt USHORT *mask,
  __out_opt PMODE_CHANGE_ACTION *actions, __out LPCWSTR *path);

static BOOL ChangeFileModeByMask(__in LPCWSTR path, USHORT mode);

static BOOL ChangeFileModeByActions(__in LPCWSTR path,
  PMODE_CHANGE_ACTION actions);

static BOOL ChangeFileMode(__in LPCWSTR path, __in_opt USHORT mode,
  __in_opt PMODE_CHANGE_ACTION actions);

static BOOL ChangeFileModeRecursively(__in LPCWSTR path, __in_opt USHORT mode,
  __in_opt PMODE_CHANGE_ACTION actions);

static USHORT ComputeNewMode(__in USHORT oldMode, __in USHORT who,
  __in USHORT op, __in USHORT perm, __in USHORT ref);


//----------------------------------------------------------------------------
// Function: Chmod
//
// Description:
//	The main method for chmod command
//
// Returns:
//	0: on success
//
// Notes:
//
int Chmod(int argc, wchar_t *argv[])
{
  LPWSTR pathName = NULL;
  LPWSTR longPathName = NULL;

  BOOL recursive = FALSE;

  PMODE_CHANGE_ACTION actions = NULL;

  USHORT unixAccessMask = 0x0000;

  DWORD dwRtnCode = 0;

  int ret = EXIT_FAILURE;

  // Parsing chmod arguments
  //
  if (!ParseCommandLineArguments(argc, argv,
    &recursive, &unixAccessMask, &actions, &pathName))
  {
    fwprintf(stderr, L"Incorrect command line arguments.\n\n");
    ChmodUsage(argv[0]);
    return EXIT_FAILURE;
  }

  // Convert the path the the long path
  //
  dwRtnCode = ConvertToLongPath(pathName, &longPathName);
  if (dwRtnCode != ERROR_SUCCESS)
  {
    ReportErrorCode(L"ConvertToLongPath", dwRtnCode);
    goto ChmodEnd;
  }

  if (!recursive)
  {
    if (ChangeFileMode(longPathName, unixAccessMask, actions))
    {
      ret = EXIT_SUCCESS;
    }
  }
  else
  {
    if (ChangeFileModeRecursively(longPathName, unixAccessMask, actions))
    {
      ret = EXIT_SUCCESS;
    }
  }

ChmodEnd:
  FreeActions(actions);
  LocalFree(longPathName);

  return ret;
}

//----------------------------------------------------------------------------
// Function: ChangeFileMode
//
// Description:
//	Wrapper function for change file mode. Choose either change by action or by
//  access mask.
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//
static BOOL ChangeFileMode(__in LPCWSTR path, __in_opt USHORT unixAccessMask,
  __in_opt PMODE_CHANGE_ACTION actions)
{
  if (actions != NULL)
    return ChangeFileModeByActions(path, actions);
  else
    return ChangeFileModeByMask(path, unixAccessMask);
}

//----------------------------------------------------------------------------
// Function: ChangeFileModeRecursively
//
// Description:
//	Travel the directory recursively to change the permissions.
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//  The recursion works in the following way:
//    - If the path is not a directory, change its mode and return;
//    - Otherwise, call the method on all its children, then change its mode.
//
static BOOL ChangeFileModeRecursively(__in LPCWSTR path, __in_opt USHORT mode,
  __in_opt PMODE_CHANGE_ACTION actions)
{
  LPWSTR dir = NULL;

  size_t pathSize = 0;
  size_t dirSize = 0;

  HANDLE hFind = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATA ffd;
  DWORD dwRtnCode = ERROR_SUCCESS;
  BOOL ret = FALSE;

  BY_HANDLE_FILE_INFORMATION fileInfo;

  if ((dwRtnCode = GetFileInformationByName(path, &fileInfo)) != ERROR_SUCCESS)
  {
    ReportErrorCode(L"GetFileInformationByName", dwRtnCode);
    return FALSE;
  }

  if (!IsDirFileInfo(&fileInfo))
  {
     if (ChangeFileMode(path, mode, actions))
     {
       return TRUE;
     }
     else
     {
       return FALSE;
     }
  }

  // MAX_PATH is used here, because we use relative path, and relative
  // paths are always limited to a total of MAX_PATH characters.
  //
  if (FAILED(StringCchLengthW(path, MAX_PATH - 3, &pathSize)))
  {
    return FALSE;
  }
  dirSize = pathSize + 3;
  dir = (LPWSTR)LocalAlloc(LPTR, dirSize * sizeof(WCHAR));
  if (dir == NULL)
  {
    ReportErrorCode(L"LocalAlloc", GetLastError());
    goto ChangeFileModeRecursivelyEnd;
  }

  if (FAILED(StringCchCopyW(dir, dirSize, path)) ||
    FAILED(StringCchCatW(dir, dirSize, L"\\*")))
  {
    goto ChangeFileModeRecursivelyEnd;
  }

  hFind = FindFirstFile(dir, &ffd);
  if (hFind == INVALID_HANDLE_VALUE)
  {
    ReportErrorCode(L"FindFirstFile", GetLastError());
    goto ChangeFileModeRecursivelyEnd;
  }

  do
  {
    LPWSTR filename = NULL;
    size_t filenameSize = 0;

    if (wcscmp(ffd.cFileName, L".") == 0 ||
      wcscmp(ffd.cFileName, L"..") == 0)
      continue;

    filenameSize = pathSize + wcslen(ffd.cFileName) + 2;
    filename = (LPWSTR)LocalAlloc(LPTR, filenameSize * sizeof(WCHAR));
    if (filename == NULL)
    {
      ReportErrorCode(L"LocalAlloc", GetLastError());
      goto ChangeFileModeRecursivelyEnd;
    }

    if (FAILED(StringCchCopyW(filename, filenameSize, path)) ||
      FAILED(StringCchCatW(filename, filenameSize, L"\\")) ||
      FAILED(StringCchCatW(filename, filenameSize, ffd.cFileName)))
    {
      LocalFree(filename);
      goto ChangeFileModeRecursivelyEnd;
    }
     
    if(!ChangeFileModeRecursively(filename, mode, actions))
    {
      LocalFree(filename);
      goto ChangeFileModeRecursivelyEnd;
    }

    LocalFree(filename);

  } while (FindNextFileW(hFind, &ffd));

  if (!ChangeFileMode(path, mode, actions))
  {
    goto ChangeFileModeRecursivelyEnd;
  }

  ret = TRUE;

ChangeFileModeRecursivelyEnd:
  LocalFree(dir);

  return ret;
}

//----------------------------------------------------------------------------
// Function: ChangeFileModeByMask
//
// Description:
//	Change a file or direcotry at the path to Unix mode
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//
static BOOL ChangeFileModeByMask(__in LPCWSTR path, USHORT mode)
{
  PACL pOldDACL = NULL;
  PACL pNewDACL = NULL;
  PSID pOwnerSid = NULL;
  PSID pGroupSid = NULL;
  PSECURITY_DESCRIPTOR pSD = NULL;

  SECURITY_DESCRIPTOR_CONTROL control;
  DWORD revision = 0;

  PSECURITY_DESCRIPTOR pAbsSD = NULL;
  PACL pAbsDacl = NULL;
  PACL pAbsSacl = NULL;
  PSID pAbsOwner = NULL;
  PSID pAbsGroup = NULL;

  DWORD dwRtnCode = 0;
  DWORD dwErrorCode = 0;

  BOOL ret = FALSE;

  // Get owner and group Sids
  //
  dwRtnCode = GetNamedSecurityInfoW(
    path,
    SE_FILE_OBJECT, 
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
    DACL_SECURITY_INFORMATION,
    &pOwnerSid,
    &pGroupSid,
    &pOldDACL,
    NULL,
    &pSD);
  if (ERROR_SUCCESS != dwRtnCode)
  {
    ReportErrorCode(L"GetNamedSecurityInfo", dwRtnCode);
    goto ChangeFileMode; 
  }

  // SetSecurityDescriptorDacl function used below only accepts security
  // descriptor in absolute format, meaning that its members must be pointers to
  // other structures, rather than offsets to contiguous data.
  // To determine whether a security descriptor is self-relative or absolute,
  // call the GetSecurityDescriptorControl function and check the
  // SE_SELF_RELATIVE flag of the SECURITY_DESCRIPTOR_CONTROL parameter.
  //
  if (!GetSecurityDescriptorControl(pSD, &control, &revision))
  {
    ReportErrorCode(L"GetSecurityDescriptorControl", GetLastError());
    goto ChangeFileMode;
  }

  // If the security descriptor is self-relative, we use MakeAbsoluteSD function
  // to convert it to absolute format.
  //
  if ((control & SE_SELF_RELATIVE) == SE_SELF_RELATIVE)
  {
    DWORD absSDSize = 0;
    DWORD daclSize = 0;
    DWORD saclSize = 0;
    DWORD ownerSize = 0;
    DWORD primaryGroupSize = 0;
    MakeAbsoluteSD(pSD, NULL, &absSDSize, NULL, &daclSize, NULL,
      &saclSize, NULL, &ownerSize, NULL, &primaryGroupSize);
    if ((dwErrorCode = GetLastError()) != ERROR_INSUFFICIENT_BUFFER)
    {
      ReportErrorCode(L"MakeAbsoluteSD", dwErrorCode);
      goto ChangeFileMode;
    }
    pAbsSD = (PSECURITY_DESCRIPTOR) LocalAlloc(LPTR, absSDSize);
    pAbsDacl = (PACL) LocalAlloc(LPTR, daclSize);
    pAbsSacl = (PACL) LocalAlloc(LPTR, saclSize);
    pAbsOwner = (PSID) LocalAlloc(LPTR, ownerSize);
    pAbsGroup = (PSID) LocalAlloc(LPTR, primaryGroupSize);
    if (pAbsSD == NULL || pAbsDacl == NULL || pAbsSacl == NULL ||
      pAbsOwner == NULL || pAbsGroup == NULL)
    {
      ReportErrorCode(L"LocalAlloc", GetLastError());
      goto ChangeFileMode;
    }

    if (!MakeAbsoluteSD(pSD, pAbsSD, &absSDSize, pAbsDacl, &daclSize, pAbsSacl,
      &saclSize, pAbsOwner, &ownerSize, pAbsGroup, &primaryGroupSize))
    {
      ReportErrorCode(L"MakeAbsoluteSD", GetLastError());
      goto ChangeFileMode;
    }
  }

  // Get Windows DACLs based on Unix access mask
  //
  if (!GetWindowsDACLs(mode, pOwnerSid, pGroupSid, &pNewDACL))
    goto ChangeFileMode;

  // Set the DACL information in the security descriptor; if a DACL is already
  // present in the security descriptor, the DACL is replaced. The security
  // descriptor is then used to set the security of a file or directory.
  //
  if (!SetSecurityDescriptorDacl(pAbsSD, TRUE, pNewDACL, FALSE))
  {
    ReportErrorCode(L"SetSecurityDescriptorDacl", GetLastError());
    goto ChangeFileMode;
  }

  // MSDN states "This function is obsolete. Use the SetNamedSecurityInfo
  // function instead." However we have the following problem when using
  // SetNamedSecurityInfo:
  //  - When PROTECTED_DACL_SECURITY_INFORMATION is not passed in as part of
  //    security information, the object will include inheritable permissions
  //    from its parent.
  //  - When PROTECTED_DACL_SECURITY_INFORMATION is passsed in to set
  //    permissions on a directory, the child object of the directory will lose
  //    inheritable permissions from their parent (the current directory).
  // By using SetFileSecurity, we have the nice property that the new
  // permissions of the object does not include the inheritable permissions from
  // its parent, and the child objects will not lose their inherited permissions
  // from the current object.
  //
  if (!SetFileSecurity(path, DACL_SECURITY_INFORMATION, pAbsSD))
  {
    ReportErrorCode(L"SetFileSecurity", GetLastError());
    goto ChangeFileMode;
  }

  ret = TRUE;

ChangeFileMode:
  LocalFree(pSD);
  LocalFree(pNewDACL);
  LocalFree(pAbsDacl);
  LocalFree(pAbsSacl);
  LocalFree(pAbsOwner);
  LocalFree(pAbsGroup);
  LocalFree(pAbsSD);

  return ret;
}

//----------------------------------------------------------------------------
// Function: ParseCommandLineArguments
//
// Description:
//	Parse command line arguments for chmod.
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//	1. Recursive is only set on directories
//  2. 'actions' is NULL if the mode is octal
//
static BOOL ParseCommandLineArguments(__in int argc, __in wchar_t *argv[],
  __out BOOL *rec,
  __out_opt USHORT *mask,
  __out_opt PMODE_CHANGE_ACTION *actions,
  __out LPCWSTR *path)
{
  LPCWSTR maskString;
  BY_HANDLE_FILE_INFORMATION fileInfo;
  DWORD dwRtnCode = ERROR_SUCCESS;

  assert(path != NULL);

  if (argc != 3 && argc != 4)
    return FALSE;

  *rec = FALSE;
  if (argc == 4)
  {
    maskString = argv[2];
    *path = argv[3];

    if (wcscmp(argv[1], L"-R") == 0)
    {
      // Check if the given path name is a file or directory
      // Only set recursive flag if the given path is a directory
      //
      dwRtnCode = GetFileInformationByName(*path, &fileInfo);
      if (dwRtnCode != ERROR_SUCCESS)
      {
        ReportErrorCode(L"GetFileInformationByName", dwRtnCode);
        return FALSE;
      }

      if (IsDirFileInfo(&fileInfo))
      {
        *rec = TRUE;
      }
    }
    else
      return FALSE;
  }
  else
  {
    maskString = argv[1];
    *path = argv[2];
  }

  if (ParseOctalMode(maskString, mask))
  {
    return TRUE;
  }
  else if (ParseMode(maskString, actions))
  {
    return TRUE;
  }

  return FALSE;
}

//----------------------------------------------------------------------------
// Function: FreeActions
//
// Description:
//	Free a linked list of mode change actions given the head node.
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//	none
//
static BOOL FreeActions(PMODE_CHANGE_ACTION actions)
{
  PMODE_CHANGE_ACTION curr = NULL;
  PMODE_CHANGE_ACTION next = NULL;
  
  // Nothing to free if NULL is passed in
  //
  if (actions == NULL)
  {
    return TRUE;
  }

  curr = actions;
  while (curr != NULL)
  {
    next = curr->next_action;
    LocalFree(curr);
    curr = next;
  }
  actions = NULL;

  return TRUE;
}

//----------------------------------------------------------------------------
// Function: ComputeNewMode
//
// Description:
//	Compute a new mode based on the old mode and a mode change action.
//
// Returns:
//	The newly computed mode
//
// Notes:
//	Apply 'rwx' permission mask or reference permission mode according to the
//  '+', '-', or '=' operator.
//
static USHORT ComputeNewMode(__in USHORT oldMode,
  __in USHORT who, __in USHORT op,
  __in USHORT perm, __in USHORT ref)
{
  static const USHORT readMask  = 0444;
  static const USHORT writeMask = 0222;
  static const USHORT exeMask   = 0111;

  USHORT mask = 0;
  USHORT mode = 0;

  // Operation and reference mode are exclusive
  //
  assert(op == CHMOD_OP_EQUAL || op == CHMOD_OP_PLUS || op == CHMOD_OP_MINUS);
  assert(ref == CHMOD_WHO_GROUP || ref == CHMOD_WHO_USER ||
    ref == CHMOD_WHO_OTHER);

  if(perm == CHMOD_PERM_NA && ref == CHMOD_WHO_NONE)
  {
    return oldMode;
  }

  if (perm != CHMOD_PERM_NA)
  {
    if ((perm & CHMOD_PERM_R) == CHMOD_PERM_R)
      mask |= readMask;
    if ((perm & CHMOD_PERM_W) == CHMOD_PERM_W)
      mask |= writeMask;
    if ((perm & CHMOD_PERM_X) == CHMOD_PERM_X)
      mask |= exeMask;
    if (((perm & CHMOD_PERM_LX) == CHMOD_PERM_LX))
    {
      // It applies execute permissions to directories regardless of their
      // current permissions and applies execute permissions to a file which
      // already has at least 1 execute permission bit already set (either user,
      // group or other). It is only really useful when used with '+' and
      // usually in combination with the -R option for giving group or other
      // access to a big directory tree without setting execute permission on
      // normal files (such as text files), which would normally happen if you
      // just used "chmod -R a+rx .", whereas with 'X' you can do
      // "chmod -R a+rX ." instead (Source: Wikipedia)
      //
      if ((oldMode & UX_DIRECTORY) == UX_DIRECTORY || (oldMode & exeMask))
        mask |= exeMask;
    }
  }

  mask |= oldMode & ref;

  mask &= who;

  if (op == CHMOD_OP_EQUAL)
  {
    mode = mask;
  }
  else if (op == CHMOD_OP_MINUS)
  {
    mode = oldMode & (~mask);
  }
  else if (op == CHMOD_OP_PLUS)
  {
    mode = oldMode | mask;
  }

  return mode;
}

//----------------------------------------------------------------------------
// Function: ConvertActionsToMask
//
// Description:
//	Convert a linked list of mode change actions to the Unix permission mask
//  given the head node.
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//	none
//
static BOOL ConvertActionsToMask(__in LPCWSTR path,
  __in PMODE_CHANGE_ACTION actions, __out PUSHORT puMask)
{
  PMODE_CHANGE_ACTION curr = NULL;

  BY_HANDLE_FILE_INFORMATION fileInformation;
  DWORD dwErrorCode = ERROR_SUCCESS;

  USHORT mode = 0;

  dwErrorCode = GetFileInformationByName(path, &fileInformation);
  if (dwErrorCode != ERROR_SUCCESS)
  {
    ReportErrorCode(L"GetFileInformationByName", dwErrorCode);
    return FALSE;
  }
  if (IsDirFileInfo(&fileInformation))
  {
    mode |= UX_DIRECTORY;
  }
  if (!FindFileOwnerAndPermission(path, NULL, NULL, &mode))
  {
    return FALSE;
  }
  *puMask = mode;

  // Nothing to change if NULL is passed in
  //
  if (actions == NULL)
  {
    return TRUE;
  }

  for (curr = actions; curr != NULL; curr = curr->next_action)
  {
    mode = ComputeNewMode(mode, curr->who, curr->op, curr->perm, curr->ref);
  }

  *puMask = mode;
  return TRUE;
}

//----------------------------------------------------------------------------
// Function: ChangeFileModeByActions
//
// Description:
//	Change a file mode through a list of actions.
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//	none
//
static BOOL ChangeFileModeByActions(__in LPCWSTR path,
  PMODE_CHANGE_ACTION actions)
{
  USHORT mask = 0;

  if (ConvertActionsToMask(path, actions, &mask))
    return ChangeFileModeByMask(path, mask);
  else
    return FALSE;
}

//----------------------------------------------------------------------------
// Function: ParseMode
//
// Description:
//	Convert a mode string into a linked list of actions
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//	Take a state machine approach to parse the mode. Each mode change action
//  will be a node in the output linked list. The state machine has five state,
//  and each will only transit to the next; the end state can transit back to
//  the first state, and thus form a circle. In each state, if we see a
//  a character not belongs to the state, we will move to next state. WHO, PERM,
//  and REF states are optional; OP and END states are required; and errors
//  will only be reported at the latter two states.
//
static BOOL ParseMode(LPCWSTR modeString, PMODE_CHANGE_ACTION *pActions)
{
  enum __PARSE_MODE_ACTION_STATE
  {
    PARSE_MODE_ACTION_WHO_STATE,
    PARSE_MODE_ACTION_OP_STATE,
    PARSE_MODE_ACTION_PERM_STATE,
    PARSE_MODE_ACTION_REF_STATE,
    PARSE_MODE_ACTION_END_STATE
  } state = PARSE_MODE_ACTION_WHO_STATE;

  MODE_CHANGE_ACTION action = INIT_MODE_CHANGE_ACTION;
  PMODE_CHANGE_ACTION actionsEnd = NULL;
  PMODE_CHANGE_ACTION actionsLast = NULL;
  USHORT lastWho;
  WCHAR c = 0;
  size_t len = 0;
  size_t i = 0;

  assert(modeString != NULL && pActions != NULL);

  if (FAILED(StringCchLengthW(modeString, STRSAFE_MAX_CCH, &len)))
  {
    return FALSE;
  }

  actionsEnd = *pActions;
  while(i <= len)
  {
    c = modeString[i];
    if (state == PARSE_MODE_ACTION_WHO_STATE)
    {
      switch (c)
      {
      case L'a':
        action.who |= CHMOD_WHO_ALL;
        i++;
        break;
      case L'u':
        action.who |= CHMOD_WHO_USER;
        i++;
        break;
      case L'g':
        action.who |= CHMOD_WHO_GROUP;
        i++;
        break;
      case L'o':
        action.who |= CHMOD_WHO_OTHER;
        i++;
        break;
      default:
        state = PARSE_MODE_ACTION_OP_STATE;
      } // WHO switch
    }
    else if (state == PARSE_MODE_ACTION_OP_STATE)
    {
      switch (c)
      {
      case L'+':
        action.op = CHMOD_OP_PLUS;
        break;
      case L'-':
        action.op = CHMOD_OP_MINUS;
        break;
      case L'=':
        action.op = CHMOD_OP_EQUAL;
        break;
      default:
        fwprintf(stderr, L"Invalid mode: '%s'\n", modeString);
        FreeActions(*pActions);
        return FALSE;
      } // OP switch
      i++;
      state = PARSE_MODE_ACTION_PERM_STATE;
    }
    else if (state == PARSE_MODE_ACTION_PERM_STATE)
    {
      switch (c)
      {
      case L'r':
        action.perm |= CHMOD_PERM_R;
        i++;
        break;
      case L'w':
        action.perm |= CHMOD_PERM_W;
        i++;
        break;
      case L'x':
        action.perm |= CHMOD_PERM_X;
        i++;
        break;
      case L'X':
        action.perm |= CHMOD_PERM_LX;
        i++;
        break;
      default:
        state = PARSE_MODE_ACTION_REF_STATE;
      } // PERM switch
    }
    else if (state == PARSE_MODE_ACTION_REF_STATE)
    {
      switch (c)
      {
      case L'u':
        action.ref = CHMOD_WHO_USER;
        i++;
        break;
      case L'g':
        action.ref = CHMOD_WHO_GROUP;
        i++;
        break;
      case L'o':
        action.ref = CHMOD_WHO_OTHER;
        i++;
        break;
      default:
        state = PARSE_MODE_ACTION_END_STATE;
      } // REF switch
    }
    else if (state == PARSE_MODE_ACTION_END_STATE)
    {
      switch (c)
      {
      case NULL:
      case L',':
        i++;
      case L'+':
      case L'-':
      case L'=':
        state = PARSE_MODE_ACTION_WHO_STATE;

        // Append the current action to the end of the linked list
        //
        assert(actionsEnd == NULL);
        // Allocate memory
        actionsEnd = (PMODE_CHANGE_ACTION) LocalAlloc(LPTR,
          sizeof(MODE_CHANGE_ACTION));
        if (actionsEnd == NULL)
        {
          ReportErrorCode(L"LocalAlloc", GetLastError());
          FreeActions(*pActions);
          return FALSE;
        }
        if (action.who == CHMOD_WHO_NONE) action.who = CHMOD_WHO_ALL;
        // Copy the action to the new node
        *actionsEnd = action;
        // Append to the last node in the linked list
        if (actionsLast != NULL) actionsLast->next_action = actionsEnd;
        // pActions should point to the head of the linked list
        if (*pActions == NULL) *pActions = actionsEnd;
        // Update the two pointers to point to the last node and the tail
        actionsLast = actionsEnd;
        actionsEnd = actionsLast->next_action;

        // Reset action
        //
        lastWho = action.who;
        action = INIT_MODE_CHANGE_ACTION;
        if (c != L',')
        {
          action.who = lastWho;
        }

        break;
      default:
        fwprintf(stderr, L"Invalid mode: '%s'\n", modeString);
        FreeActions(*pActions);
        return FALSE;
      } // END switch
    }
  } // while
  return TRUE;
}

//----------------------------------------------------------------------------
// Function: ParseOctalMode
//
// Description:
//	Convert the 3 or 4 digits Unix mask string into the binary representation
//  of the Unix access mask, i.e. 9 bits each an indicator of the permission
//  of 'rwxrwxrwx', i.e. user's, group's, and owner's read, write, and
//  execute/search permissions.
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//	none
//
static BOOL ParseOctalMode(LPCWSTR tsMask, USHORT *uMask)
{
  size_t tsMaskLen = 0;
  DWORD i;
  LONG l;
  WCHAR *end;

  if (uMask == NULL)
    return FALSE;

  if (FAILED(StringCchLengthW(tsMask, STRSAFE_MAX_CCH, &tsMaskLen)))
    return FALSE;

  if (tsMaskLen != 4 && tsMaskLen != 3)
  {
    return FALSE;
  }

  for (i = 0; i < tsMaskLen; i++)
  {
    if (!(tsMask[tsMaskLen - i - 1] >= L'0' &&
      tsMask[tsMaskLen - i - 1] <= L'7'))
      return FALSE;
  }

  errno = 0;
  if (tsMaskLen == 4)
    // Windows does not have any equivalent of setuid/setgid and sticky bit.
    // So the first bit is omitted for the 4 digit octal mode case.
    //
    l = wcstol(tsMask + 1, &end, 8);
  else
    l = wcstol(tsMask, &end, 8);

  if (errno || l > 0x0777 || l < 0 || *end != 0)
  {
    return FALSE;
  }

  *uMask = (USHORT) l;

  return TRUE;
}

//----------------------------------------------------------------------------
// Function: GetWindowsAccessMask
//
// Description:
//	Get the Windows AccessMask for user, group and everyone based on the Unix
//  permission mask
//
// Returns:
//	none
//
// Notes:
//	none
//
static void GetWindowsAccessMask(USHORT unixMask,
  ACCESS_MASK *userAllow,
  ACCESS_MASK *userDeny,
  ACCESS_MASK *groupAllow,
  ACCESS_MASK *groupDeny,
  ACCESS_MASK *otherAllow)
{
  assert (userAllow != NULL && userDeny != NULL &&
    groupAllow != NULL && groupDeny != NULL &&
    otherAllow != NULL);

  *userAllow = WinMasks[WIN_ALL] | WinMasks[WIN_OWNER_SE];
  if ((unixMask & UX_U_READ) == UX_U_READ)
    *userAllow |= WinMasks[WIN_READ];

  if ((unixMask & UX_U_WRITE) == UX_U_WRITE)
    *userAllow |= WinMasks[WIN_WRITE];

  if ((unixMask & UX_U_EXECUTE) == UX_U_EXECUTE)
    *userAllow |= WinMasks[WIN_EXECUTE];

  *userDeny = 0;
  if ((unixMask & UX_U_READ) != UX_U_READ &&
    ((unixMask & UX_G_READ) == UX_G_READ ||
    (unixMask & UX_O_READ) == UX_O_READ))
    *userDeny |= WinMasks[WIN_READ];

  if ((unixMask & UX_U_WRITE) != UX_U_WRITE &&
    ((unixMask & UX_G_WRITE) == UX_G_WRITE ||
    (unixMask & UX_O_WRITE) == UX_O_WRITE))
    *userDeny |= WinMasks[WIN_WRITE];

  if ((unixMask & UX_U_EXECUTE) != UX_U_EXECUTE &&
    ((unixMask & UX_G_EXECUTE) == UX_G_EXECUTE ||
    (unixMask & UX_O_EXECUTE) == UX_O_EXECUTE))
    *userDeny |= WinMasks[WIN_EXECUTE];

  *groupAllow = WinMasks[WIN_ALL];
  if ((unixMask & UX_G_READ) == UX_G_READ)
    *groupAllow |= FILE_GENERIC_READ;

  if ((unixMask & UX_G_WRITE) == UX_G_WRITE)
    *groupAllow |= WinMasks[WIN_WRITE];

  if ((unixMask & UX_G_EXECUTE) == UX_G_EXECUTE)
    *groupAllow |= WinMasks[WIN_EXECUTE];

  *groupDeny = 0;
  if ((unixMask & UX_G_READ) != UX_G_READ &&
    (unixMask & UX_O_READ) == UX_O_READ)
    *groupDeny |= WinMasks[WIN_READ];

  if ((unixMask & UX_G_WRITE) != UX_G_WRITE &&
    (unixMask & UX_O_WRITE) == UX_O_WRITE)
    *groupDeny |= WinMasks[WIN_WRITE];

  if ((unixMask & UX_G_EXECUTE) != UX_G_EXECUTE &&
    (unixMask & UX_O_EXECUTE) == UX_O_EXECUTE)
    *groupDeny |= WinMasks[WIN_EXECUTE];

  *otherAllow = WinMasks[WIN_ALL];
  if ((unixMask & UX_O_READ) == UX_O_READ)
    *otherAllow |= WinMasks[WIN_READ];

  if ((unixMask & UX_O_WRITE) == UX_O_WRITE)
    *otherAllow |= WinMasks[WIN_WRITE];

  if ((unixMask & UX_O_EXECUTE) == UX_O_EXECUTE)
    *otherAllow |= WinMasks[WIN_EXECUTE];
}

//----------------------------------------------------------------------------
// Function: GetWindowsDACLs
//
// Description:
//	Get the Windows DACs based the Unix access mask
//
// Returns:
//	TRUE: on success
//  FALSE: otherwise
//
// Notes:
//	none
//
static BOOL GetWindowsDACLs(__in USHORT unixMask,
  __in PSID pOwnerSid, __in PSID pGroupSid, __out PACL *ppNewDACL)
{
  DWORD winUserAccessDenyMask;
  DWORD winUserAccessAllowMask;
  DWORD winGroupAccessDenyMask;
  DWORD winGroupAccessAllowMask;
  DWORD winOtherAccessAllowMask;

  PSID pEveryoneSid = NULL;

  PACL pNewDACL = NULL;
  DWORD dwNewAclSize = 0;

  SID_IDENTIFIER_AUTHORITY SIDAuthWorld = SECURITY_WORLD_SID_AUTHORITY;

  BOOL ret = FALSE;

  GetWindowsAccessMask(unixMask,
    &winUserAccessAllowMask, &winUserAccessDenyMask,
    &winGroupAccessAllowMask, &winGroupAccessDenyMask,
    &winOtherAccessAllowMask);

  // Create a well-known SID for the Everyone group
  //
  if(!AllocateAndInitializeSid(&SIDAuthWorld, 1,
    SECURITY_WORLD_RID,
    0, 0, 0, 0, 0, 0, 0,
    &pEveryoneSid))
  {
    return FALSE;
  }

  // Create the new DACL
  //
  dwNewAclSize = sizeof(ACL);
  dwNewAclSize += sizeof(ACCESS_ALLOWED_ACE) +
    GetLengthSid(pOwnerSid) - sizeof(DWORD);
  if (winUserAccessDenyMask)
    dwNewAclSize += sizeof(ACCESS_DENIED_ACE) +
    GetLengthSid(pOwnerSid) - sizeof(DWORD);
  dwNewAclSize += sizeof(ACCESS_ALLOWED_ACE) +
    GetLengthSid(pGroupSid) - sizeof(DWORD);
  if (winGroupAccessDenyMask)
    dwNewAclSize += sizeof(ACCESS_DENIED_ACE) +
    GetLengthSid(pGroupSid) - sizeof(DWORD);
  dwNewAclSize += sizeof(ACCESS_ALLOWED_ACE) +
    GetLengthSid(pEveryoneSid) - sizeof(DWORD);
  pNewDACL = (PACL)LocalAlloc(LPTR, dwNewAclSize);
  if (pNewDACL == NULL)
  {
    ReportErrorCode(L"LocalAlloc", GetLastError());
    goto GetWindowsDACLsEnd;
  }
  if (!InitializeAcl(pNewDACL, dwNewAclSize, ACL_REVISION))
  {
    ReportErrorCode(L"InitializeAcl", GetLastError());
    goto GetWindowsDACLsEnd;
  }

  if (winUserAccessDenyMask &&
    !AddAccessDeniedAce(pNewDACL, ACL_REVISION,
    winUserAccessDenyMask, pOwnerSid))
  {
    ReportErrorCode(L"AddAccessDeniedAce", GetLastError());
    goto GetWindowsDACLsEnd;
  }
  if (!AddAccessAllowedAce(pNewDACL, ACL_REVISION,
    winUserAccessAllowMask, pOwnerSid))
  {
    ReportErrorCode(L"AddAccessAllowedAce", GetLastError());
    goto GetWindowsDACLsEnd;
  }
  if (winGroupAccessDenyMask &&
    !AddAccessDeniedAce(pNewDACL, ACL_REVISION,
    winGroupAccessDenyMask, pGroupSid))
  {
    ReportErrorCode(L"AddAccessDeniedAce", GetLastError());
    goto GetWindowsDACLsEnd;
  }
  if (!AddAccessAllowedAce(pNewDACL, ACL_REVISION,
    winGroupAccessAllowMask, pGroupSid))
  {
    ReportErrorCode(L"AddAccessAllowedAce", GetLastError());
    goto GetWindowsDACLsEnd;
  }
  if (!AddAccessAllowedAce(pNewDACL, ACL_REVISION,
    winOtherAccessAllowMask, pEveryoneSid))
  {
    ReportErrorCode(L"AddAccessAllowedAce", GetLastError());
    goto GetWindowsDACLsEnd;
  }

  *ppNewDACL = pNewDACL;
  ret = TRUE;

GetWindowsDACLsEnd:
  if (pEveryoneSid) FreeSid(pEveryoneSid);
  if (!ret) LocalFree(pNewDACL);
  
  return ret;
}

void ChmodUsage(LPCWSTR program)
{
  fwprintf(stdout, L"\
Usage: %s [OPTION] OCTAL-MODE [FILE]\n\
   or: %s [OPTION] MODE [FILE]\n\
Change the mode of the FILE to MODE.\n\
\n\
   -R: change files and directories recursively\n\
\n\
Each MODE is of the form '[ugoa]*([-+=]([rwxX]*|[ugo]))+'.\n",
program, program);
}