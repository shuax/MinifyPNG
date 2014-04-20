#define UNICODE
#define _UNICODE
#define NDEBUG
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tchar.h>
#include <stdio.h>
#include <shlobj.h>
#include <process.h>
#include "resource.h"



void MyListBox_AddString(HWND list, const wchar_t *file)
{
    ListBox_AddString(list, file);
    ListBox_SetCaretIndex(list, ListBox_GetCount(list)-1);
}


#define ListBox_AddString MyListBox_AddString

#include "MinifyPNG.h"

HINSTANCE hInst;
HWND m_hwnd = 0;
HWND list_box = 0;
HWND check_box = 0;
HWND Progressbar = 0;

bool isEndWith(const wchar_t *path,const wchar_t* ext)
{
	if(!path || !ext) return false;
    int len1 = wcslen(path);
    int len2 = wcslen(ext);
    if(len2>len1) return false;
    return !_memicmp(path + len1 - len2,ext,len2*sizeof(wchar_t));
}

struct files_
{
    wchar_t file_name[MAX_PATH];
};

files_ *files = 0;
int files_num = 0;
void ResetFiles()
{
    if(files)
    {
        free(files);
        files = 0;
    }
    files_num = 0;
}
void AppendFiles(const wchar_t *file)
{
    files_num++;
    files = (files_*)realloc(files, sizeof(files_)*files_num );
    memcpy(files[files_num-1].file_name,file,sizeof(files_));
}

void FindFileInDir(const wchar_t *szFilename,int dept)
{
    if(dept==4) return;

    wchar_t temp[MAX_PATH];
    _tcscpy(temp, szFilename);
    _tcscat(temp, _T("\\*.*"));

    WIN32_FIND_DATA ffbuf;
    HANDLE hfind = FindFirstFile(temp, &ffbuf);
    if (hfind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if( isEndWith(ffbuf.cFileName, _T(".png")))
            {
                _tcscpy(temp, szFilename);
                _tcscat(temp, _T("\\"));
                _tcscat(temp, ffbuf.cFileName);

                //MinifyPNG(list_box, temp);
                AppendFiles(temp);
            }
            else
            {
                if( (ffbuf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(ffbuf.cFileName,L".") && wcscmp(ffbuf.cFileName,L"..") )
                {
                    //
                    _tcscpy(temp, szFilename);
                    _tcscat(temp, _T("\\"));
                    _tcscat(temp, ffbuf.cFileName);
                    FindFileInDir(temp, dept+1);
                }
            }
        }
        while (FindNextFile(hfind, &ffbuf));
        FindClose(hfind);
    }
}
void DoDropFiles(LPVOID pvoid)
{
    static int running = false;

    if(!running)
    {
        running = true;
        HDROP hDrop = (HDROP)pvoid;
        int nNumFiles = DragQueryFile(hDrop, -1, NULL, 0);

        ListBox_ResetContent(list_box);
        ResetFiles();

        for(int i=0;i<nNumFiles;i++)
        {
            wchar_t szFilename[MAX_PATH];
            DragQueryFile(hDrop, i, szFilename, MAX_PATH);

            if( GetFileAttributes(szFilename) & FILE_ATTRIBUTE_DIRECTORY)
            {
                FindFileInDir(szFilename, 1);
            }
            else
            {
                //
                AppendFiles(szFilename);
            }
        }

        SendMessage(Progressbar, PBM_SETRANGE, 0, (LPARAM)(MAKELPARAM(0,files_num)));
        SendMessage(Progressbar, PBM_SETPOS, 0, 0);

        bool save_bak = Button_GetCheck(check_box);
        for(int i=0;i<files_num;i++)
        {
            MinifyPNG(list_box, files[i].file_name, save_bak);
            SendMessage(Progressbar,PBM_SETPOS,i+1,0);
        }
        if(files_num==0)
        {
            ListBox_AddString(list_box, L"未找到PNG文件。");
            ListBox_AddString(list_box, L"");
        }

        DragFinish(hDrop);
        running = false;
        ListBox_AddString(list_box, L"全部任务已经完成。");
        ListBox_AddString(list_box, L"");
    }
    else
    {
        MessageBox(m_hwnd,L"当前有任务正在处理中，请耐心等待。",L"提示",0);
    }
}
BOOL CALLBACK DlgMain(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch(uMsg)
    {
    case WM_INITDIALOG:
    {
        SetWindowText(hwndDlg, L"MinifyPNG v1.2 - www.shuax.com");
        DragAcceptFiles(hwndDlg, true);
        m_hwnd = hwndDlg;
        list_box = GetDlgItem(hwndDlg, IDC_LST1);
        check_box = GetDlgItem(hwndDlg, IDC_CHK1);
        Progressbar = GetDlgItem(hwndDlg, IDC_PGB1);

        Button_SetCheck(check_box, true);

        LPWSTR *szArgList;
        int argCount;

        szArgList = CommandLineToArgvW(GetCommandLine(), &argCount);
        if(argCount!=1)
        {
            DWORD bufsize = sizeof(DROPFILES);
            for(int i=1;i<argCount;i++)
            {
                bufsize += ( wcslen(szArgList[i])*2 + 2);
            }
            bufsize+=2;

            BYTE *buf = (BYTE*)LocalAlloc(LPTR, bufsize);

            DROPFILES *oDropFiles = (DROPFILES*)buf;
            oDropFiles->pFiles = sizeof(DROPFILES);
            oDropFiles->fWide  = true;
            oDropFiles->fNC = FALSE;
            oDropFiles->pt.x = 10;
            oDropFiles->pt.y = 10;

            int offset = sizeof(DROPFILES);
            for(int i=1;i<argCount;i++)
            {
                int len = wcslen(szArgList[i])*2 + 2;
                memcpy(buf+offset,szArgList[i], len);
                offset += len;
            }

            SendMessage(hwndDlg, WM_DROPFILES,  (WPARAM)buf, 0);

            //LocalFree(lpBuffer);
        }
        //LocalFree(szArgList);
    }
    return TRUE;

    case WM_CLOSE:
    {
        EndDialog(hwndDlg, 0);
    }
    return TRUE;

    case WM_DROPFILES:
    {
        _beginthread(DoDropFiles,0,(LPVOID)wParam);
    }
    return TRUE;

    case WM_COMMAND:
    {
        switch(LOWORD(wParam))
        {
        }
    }
    return TRUE;
    }
    return FALSE;
}


int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    options.verbose = 0;
    options.numiterations = 15;
    options.blocksplitting = 1;
    options.blocksplittinglast = 0;
    options.blocksplittingmax = 15;

    hInst=hInstance;
    InitCommonControls();
    return DialogBox(hInst, MAKEINTRESOURCE(DLG_MAIN), NULL, (DLGPROC)DlgMain);
}
