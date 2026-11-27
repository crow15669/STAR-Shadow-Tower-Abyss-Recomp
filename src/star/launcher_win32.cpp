// The launcher's file dialog (Windows). Kept apart from launcher.cpp so that <windows.h> and raylib's names never meet.
#include "star/launcher.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>

namespace star::launcher
{
    bool pickDiscImage(void *window, std::filesystem::path &path)
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool picked = false;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
            const COMDLG_FILTERSPEC filters[] = {{L"PS2 disc image (*.iso, *.chd)", L"*.iso;*.chd"}, {L"All files", L"*.*"}};
            dialog->SetFileTypes(2, filters);
            dialog->SetTitle(L"Choose your Shadow Tower Abyss disc image");
            // start in the folder of the image chosen before
            std::error_code error;
            if (!path.empty() && std::filesystem::is_directory(path.parent_path(), error))
            {
                IShellItem *folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(path.parent_path().wstring().c_str(), nullptr, IID_PPV_ARGS(&folder))))
                {
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
            if (SUCCEEDED(dialog->Show(static_cast<HWND>(window))))
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR name = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name)
                    {
                        path = std::filesystem::path(name);
                        picked = true;
                        CoTaskMemFree(name);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return picked;
    }
}
#else
namespace star::launcher
{
    bool pickDiscImage(void *, std::filesystem::path &) { return false; } // (drop the file on the window instead)
}
#endif
