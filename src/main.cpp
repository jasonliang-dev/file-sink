#include "deps/IconsFontAwesome4.h"
#include "deps/glad2.h"
#include "deps/imgui.h"
#include "deps/imgui_impl_glfw.h"
#include "deps/imgui_impl_opengl3.h"
#include "deps/imgui_internal.h"
#include "deps/imgui_stdlib.h"
#include "language.h"
#include <filesystem>
#include <fstream>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <shobjidl.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <ws2tcpip.h>

#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_NATIVE_INCLUDE_NONE
#include <GLFW/glfw3native.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "Comdlg32.lib")

namespace fs = std::filesystem;

constexpr const char *CONFIG_PATH = "./config.txt";

struct Config {
  std::string user;
  std::string host;
  std::string priv_key;

  std::string local_dir;
  std::string remote_dir;
};

enum class FileKind : i32 {
  None,
  File,
  Dir,
};

struct File {
  std::string name;
  FileKind kind = FileKind::None;
  u64 size = 0;
};

struct Net {
  LIBSSH2_SESSION *session = nullptr;
  LIBSSH2_SFTP *sftp = nullptr;
  SOCKET sock = 0;
};

struct FileChange {
  std::string filename;
  i32 type = 0;
};

struct FileWatcher {
  OVERLAPPED overlapped = {};
  HANDLE dir = INVALID_HANDLE_VALUE;
  void *buf = nullptr;
  DWORD buf_size = 0;
  std::vector<FileChange> changes;
  std::unordered_map<std::string, i64> modtimes;

  bool running() const { return dir != INVALID_HANDLE_VALUE; }
};

struct App {
  GLFWwindow *window = nullptr;
  bool ran_first_update = false;
  bool show_demo = false;
  std::vector<File> local_working_dir;
  std::vector<File> remote_working_dir;
  std::vector<std::string> watcher_log;
};

static void error_message(const wchar_t *msg) {
  MessageBox(nullptr, msg, nullptr, 0);
}

static void watcher_init(FileWatcher *watcher,
                         const std::filesystem::path &path) {
  HANDLE dir =
      CreateFile(path.c_str(), FILE_LIST_DIRECTORY,
                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                 nullptr, OPEN_EXISTING,
                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (!dir) {
    error_message(L"cannot create directory handle for file watcher");
  }

  OVERLAPPED overlapped = {};
  overlapped.hEvent = CreateEvent(nullptr, false, false, nullptr);

  constexpr DWORD buf_size = 4096;
  void *buf = malloc(buf_size);

  bool ok = ReadDirectoryChangesW(dir, buf, buf_size, true,
                                  FILE_NOTIFY_CHANGE_FILE_NAME |
                                      FILE_NOTIFY_CHANGE_DIR_NAME |
                                      FILE_NOTIFY_CHANGE_LAST_WRITE,
                                  nullptr, &overlapped, nullptr);
  if (!ok) {
    error_message(L"ReadDirectoryChangesW failed");
  }

  watcher->dir = dir;
  watcher->overlapped = overlapped;
  watcher->buf = buf;
  watcher->buf_size = buf_size;
}

static bool watcher_destroy(FileWatcher *watcher) {
  if (!CloseHandle(watcher->overlapped.hEvent)) {
    return false;
  }

  if (!CloseHandle(watcher->dir)) {
    return false;
  }

  free(watcher->buf);

  *watcher = {};
  return true;
}

static void watcher_poll(FileWatcher *watcher) {
  watcher->changes.clear();
  if (!watcher->running()) {
    return;
  }

  if (!watcher->overlapped.hEvent) {
    return;
  }

  DWORD wait = WaitForSingleObject(watcher->overlapped.hEvent, 0);
  if (wait != WAIT_OBJECT_0) {
    return;
  }

  DWORD bytes = 0;
  GetOverlappedResult(watcher->dir, &watcher->overlapped, &bytes, false);

  auto info = (FILE_NOTIFY_INFORMATION *)watcher->buf;

  char filename[MAX_PATH] = {};

  while (true) {
    if (info->Action != 0) {
      i32 wlen = info->FileNameLength / sizeof(wchar_t);

      errno_t err = wcstombs_s(nullptr, filename, array_size(filename),
                               info->FileName, wlen);
      if (!err) {
        FileChange change = {};
        change.type = info->Action;
        change.filename = filename;
        watcher->changes.push_back(std::move(change));
      }
    }

    if (info->NextEntryOffset) {
      char *next_entry = &((char *)info)[info->NextEntryOffset];
      info = (FILE_NOTIFY_INFORMATION *)next_entry;
    } else {
      break;
    }
  }

  ReadDirectoryChangesW(watcher->dir, watcher->buf, watcher->buf_size, true,
                        FILE_NOTIFY_CHANGE_FILE_NAME |
                            FILE_NOTIFY_CHANGE_DIR_NAME |
                            FILE_NOTIFY_CHANGE_LAST_WRITE,
                        nullptr, &watcher->overlapped, nullptr);
}

static std::optional<std::string> read_entire_file(const char *path) {
  std::ifstream file;
  file.open(path);

  std::ostringstream oss;
  oss << file.rdbuf();
  return oss.str();
}

static bool read_config(Config *config) {
  auto ok = read_entire_file(CONFIG_PATH);
  if (!ok) {
    return false;
  }
  std::istringstream iss(*ok);

  std::string line;
  while (std::getline(iss, line)) {
    char key[256] = {};
    char value[512] = {};
    i32 res = sscanf_s(line.data(), "%[^=]=%[^\n]", key, (u32)array_size(key),
                       value, (u32)array_size(value));
    if (res == EOF) {
      break;
    }

    if (strcmp(key, "user") == 0) {
      config->user = value;
    } else if (strcmp(key, "host") == 0) {
      config->host = value;
    } else if (strcmp(key, "priv_key") == 0) {
      config->priv_key = value;
    } else if (strcmp(key, "local_dir") == 0) {
      config->local_dir = value;
    } else if (strcmp(key, "remote_dir") == 0) {
      config->remote_dir = value;
    }
  }

  if (config->local_dir.empty()) {
    config->local_dir = ".";
  }

  if (config->remote_dir.empty()) {
    config->remote_dir = ".";
  }

  return true;
}

static void write_config(const Config &config) {
  FILE *fp = nullptr;
  errno_t err = fopen_s(&fp, CONFIG_PATH, "w");
  if (err) {
    error_message(L"failed to open config file for writing");
  }
  defer(fclose(fp));

  fprintf(fp, "user=%s\n", config.user.data());
  fprintf(fp, "host=%s\n", config.host.data());
  fprintf(fp, "priv_key=%s\n", config.priv_key.data());
  fprintf(fp, "local_dir=%s\n", config.local_dir.data());
  fprintf(fp, "remote_dir=%s\n", config.remote_dir.data());
}

static const char *open_dialog(GLFWwindow *window, const wchar_t *filter) {
  static char s_result[MAX_PATH];

  OPENFILENAME config = {};
  config.lStructSize = sizeof(OPENFILENAME);
  config.hwndOwner = glfwGetWin32Window(window);
  config.lpstrFilter = filter;

  wchar_t path[MAX_PATH] = {};

  config.lpstrFile = path;
  config.lpstrDefExt = nullptr;
  config.nMaxFile = array_size(path);
  config.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

  if (!GetOpenFileName(&config)) {
    return nullptr;
  }

  errno_t err =
      wcstombs_s(nullptr, s_result, array_size(s_result), path, _TRUNCATE);
  if (err) {
    return nullptr;
  }

  return s_result;
}

static const char *open_directory_dialog(GLFWwindow *window) {
  static char s_result[MAX_PATH];

  IFileOpenDialog *dialog;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                                    IID_IFileOpenDialog, (void **)&dialog);
  if (!SUCCEEDED(result)) {
    return nullptr;
  }
  result = dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                              FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST |
                              FOS_NOCHANGEDIR);
  defer(dialog->Release());
  if (!SUCCEEDED(result)) {
    return nullptr;
  }

  result = dialog->Show(glfwGetWin32Window(window));
  if (!SUCCEEDED(result)) {
    return nullptr;
  }

  IShellItem *item;
  result = dialog->GetResult(&item);
  if (!SUCCEEDED(result)) {
    return nullptr;
  }

  wchar_t *path;
  result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
  defer(item->Release());

  if (!SUCCEEDED(result)) {
    return nullptr;
  }

  defer(CoTaskMemFree(path));

  errno_t err =
      wcstombs_s(nullptr, s_result, array_size(s_result), path, _TRUNCATE);
  if (err) {
    return nullptr;
  }

  return s_result;
}

static std::optional<Net> server_connect(const char *host, const char *user,
                                         const char *priv_key) {
  LIBSSH2_SESSION *session = nullptr;
  LIBSSH2_SFTP *sftp = nullptr;

  SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

  sockaddr_in sin = {};
  sin.sin_family = AF_INET;
  sin.sin_port = htons(22);

  in_addr addr = {};
  if (strcmp(host, "localhost") == 0) {
    inet_pton(AF_INET, "127.0.0.1", &addr);
  } else {
    inet_pton(AF_INET, host, &addr);
  }

  sin.sin_addr.s_addr = addr.S_un.S_addr;

  if (connect(sock, (struct sockaddr *)(&sin), sizeof(struct sockaddr_in))) {
    error_message(L"cannot connect");
    return std::nullopt;
  }

  session = libssh2_session_init();
  if (!session) {
    error_message(L"cannot create session");
    return std::nullopt;
  }

  libssh2_session_set_blocking(session, 1);
  if (libssh2_session_handshake(session, sock)) {
    error_message(L"cannot establish ssh session");
    return std::nullopt;
  }

  auto userauthlist = libssh2_userauth_list(session, user, (u32)strlen(user));

  if (!strstr(userauthlist, "publickey")) {
    error_message(L"server doesn't support publickey auth");
    return std::nullopt;
  }

  if (libssh2_userauth_publickey_fromfile(session, user, nullptr, priv_key,
                                          nullptr)) {
    error_message(L"authentication failed");
    return std::nullopt;
  }

  sftp = libssh2_sftp_init(session);
  if (!sftp) {
    error_message(L"cannot create sftp session");
    return std::nullopt;
  }

  Net net;
  net.session = session;
  net.sftp = sftp;
  net.sock = sock;
  return net;
}

static void server_disconnect(Net *net) {
  if (net->sftp) {
    libssh2_sftp_shutdown(net->sftp);
  }

  if (net->session) {
    libssh2_session_disconnect(net->session, "Normal Shutdown");
    libssh2_session_free(net->session);
  }

  if (net->sock) {
    if (shutdown(net->sock, SD_BOTH)) {
      error_message(L"failed to shutdown socket");
    }

    if (closesocket(net->sock)) {
      error_message(L"failed to close socket");
    }
  }

  net->sftp = nullptr;
  net->session = nullptr;
  net->sock = 0;
}

static std::optional<std::vector<File>> read_remote_dir(LIBSSH2_SFTP *sftp,
                                                        const char *dirname) {
  auto sftp_handle = libssh2_sftp_open_ex(sftp, dirname, (u32)strlen(dirname),
                                          0, 0, LIBSSH2_SFTP_OPENDIR);
  if (!sftp_handle) {
    return std::nullopt;
  }
  defer(libssh2_sftp_close_handle(sftp_handle));

  std::vector<File> dir;

  while (true) {
    char buf[MAX_PATH] = {};
    LIBSSH2_SFTP_ATTRIBUTES attrs = {};
    i32 len = libssh2_sftp_readdir(sftp_handle, buf, array_size(buf), &attrs);
    if (len <= 0) {
      break;
    }

    if (strcmp(buf, ".") == 0 || strcmp(buf, "..") == 0) {
      continue;
    }

    File f;
    f.name = buf;

    if (attrs.permissions & LIBSSH2_SFTP_S_IFDIR) {
      f.kind = FileKind::Dir;
    } else {
      f.kind = FileKind::File;
    }

    if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
      f.size = attrs.filesize;
    }

    dir.push_back(std::move(f));
  }

  std::sort(dir.begin(), dir.end(),
            [](auto &lhs, auto &rhs) { return lhs.name < rhs.name; });

  return dir;
}

static void change_local_dir(App *app, Config *config,
                             const std::string &path) {
  config->local_dir = path;
  app->local_working_dir.clear();

  for (auto &e : fs::directory_iterator(path)) {
    File f;
    f.name = e.path().filename().string();

    if (e.is_directory()) {
      f.kind = FileKind::Dir;
    } else {
      f.kind = FileKind::File;
    }

    f.size = e.file_size();

    app->local_working_dir.push_back(std::move(f));
  }

  write_config(*config);
}

static void change_remote_dir(App *app, Config *config, Net *net,
                              const std::string &path) {
  auto list = read_remote_dir(net->sftp, path.data());
  if (list) {
    config->remote_dir = path;
    app->remote_working_dir = *std::move(list);
    write_config(*config);
  } else {
    error_message(L"failed to read remote dir");
  }
}

static bool upload_file(Config *config, Net *net, const fs::path &filename) {
  auto remote = config->remote_dir + "/" + filename.generic_string();

  auto sftp_handle = libssh2_sftp_open_ex(
      net->sftp, remote.data(), (u32)remote.size(),
      LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
      LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP |
          LIBSSH2_SFTP_S_IROTH,
      LIBSSH2_SFTP_OPENFILE);
  if (!sftp_handle) {
    return false;
  }
  defer(libssh2_sftp_close_handle(sftp_handle));

  auto file_contents = read_entire_file(
      (config->local_dir / fs::path(filename)).string().data());
  if (!file_contents) {
    return false;
  }

  u64 len = file_contents->size();
  char *ptr = file_contents->data();
  while (true) {
    i64 read = libssh2_sftp_write(sftp_handle, ptr, len);

    if (read < 0) {
      return false;
    }

    if (read == 0) {
      return true;
    }

    ptr += read;
    len -= read;
  }
}

static void app_update(App *app, Config *config, Net *net,
                       FileWatcher *watcher) {
  ImGui::DockSpaceOverViewport(ImGui::GetMainViewport(),
                               ImGuiDockNodeFlags_PassthruCentralNode);

  if (!app->ran_first_update) {
    app->ran_first_update = true;
    ImGui::OpenPopup("connect");
  }

  auto center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("connect", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::InputText("user", &config->user);
    ImGui::InputText("host", &config->host);
    ImGui::InputText("private key", &config->priv_key);

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " browse")) {
      const char *path =
          open_dialog(app->window, L"OpenSSH private key\0*.pem\0");
      if (path != nullptr) {
        config->priv_key = path;
      }
    }

    if (ImGui::Button(ICON_FA_LINK " connect", ImVec2(120, 0))) {
      auto connect = server_connect(config->host.data(), config->user.data(),
                                    config->priv_key.data());
      if (connect) {
        *net = *connect;
        write_config(*config);

        change_local_dir(app, config, config->local_dir);
        change_remote_dir(app, config, net, config->remote_dir);

        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TIMES " exit", ImVec2(120, 0))) {
      glfwSetWindowShouldClose(app->window, 1);
    }
    ImGui::EndPopup();
  }

  if (app->show_demo) {
    ImGui::ShowDemoWindow(&app->show_demo);
  }

  if (ImGui::Begin("local")) {
    if (watcher->running()) {
      ImGui::BeginDisabled();
    }

    if (ImGui::Button(ICON_FA_FOLDER_OPEN " browse")) {
      const char *dir = open_directory_dialog(app->window);
      if (dir != nullptr) {
        change_local_dir(app, config, dir);
      }
    }

    ImGui::SameLine();

    ImGui::Text("local dir: %s", config->local_dir.data());

    if (ImGui::Button(ICON_FA_REFRESH " refresh")) {
      change_local_dir(app, config, config->local_dir);
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_LONG_ARROW_UP " up one")) {
      auto str = fs::path(config->local_dir).parent_path().string();
      if (str.empty()) {
        str = ".";
      }
      change_local_dir(app, config, str);
    }

    static ImGuiTextFilter s_filter;
    s_filter.Draw();

    if (ImGui::BeginChild("local files", ImGui::GetContentRegionAvail())) {
      for (auto &file : app->local_working_dir) {
        if (!s_filter.PassFilter(file.name.data())) {
          continue;
        }

        if (file.kind == FileKind::Dir) {
          if (ImGui::Selectable(file.name.data())) {
            auto path = (config->local_dir / fs::path(file.name)).string();
            change_local_dir(app, config, path);
            break; // change_local_dir invalidates iterator
          }
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, 0xffaaaaaa);
          ImGui::Selectable(file.name.data());
          ImGui::PopStyleColor();
        }
      }
    }
    ImGui::EndChild();

    if (watcher->running()) {
      ImGui::EndDisabled();
    }
  }
  ImGui::End();

  if (ImGui::Begin("remote")) {
    if (watcher->running()) {
      ImGui::BeginDisabled();
    }

    {
      static std::string s_remote_dir;

      if (ImGui::Button(ICON_FA_KEYBOARD_O " change")) {
        s_remote_dir = config->remote_dir;
        ImGui::OpenPopup("change remote dir");
      }

      ImVec2 center = ImGui::GetMainViewport()->GetCenter();
      ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("change remote dir")) {
        ImGui::InputText("directory", &s_remote_dir);

        if (ImGui::Button("ok", ImVec2(120, 0))) {
          change_remote_dir(app, config, net, s_remote_dir);
          ImGui::CloseCurrentPopup();
        }

        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();

        if (ImGui::Button("cancel", ImVec2(120, 0))) {
          ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
      }
    }

    ImGui::SameLine();

    ImGui::Text("remote dir: %s", config->remote_dir.data());

    if (ImGui::Button(ICON_FA_REFRESH " refresh")) {
      change_remote_dir(app, config, net, config->remote_dir);
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_LONG_ARROW_UP " up one")) {
      u64 i = config->remote_dir.find_last_of('/');
      if (i != std::string::npos) {
        change_remote_dir(app, config, net, config->remote_dir.substr(0, i));
      }
    }

    static ImGuiTextFilter s_filter;
    s_filter.Draw();

    if (ImGui::BeginChild("remote files", ImGui::GetContentRegionAvail())) {
      for (auto &file : app->remote_working_dir) {
        if (!s_filter.PassFilter(file.name.data())) {
          continue;
        }

        if (file.kind == FileKind::Dir) {
          if (ImGui::Selectable(file.name.data())) {
            change_remote_dir(app, config, net,
                              config->remote_dir + "/" + file.name);
            break;
          }
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, 0xffaaaaaa);
          ImGui::Selectable(file.name.data());
          ImGui::PopStyleColor();
        }
      }
    }
    ImGui::EndChild();

    if (watcher->running()) {
      ImGui::EndDisabled();
    }
  }
  ImGui::End();

  if (ImGui::Begin("watcher")) {
    if (!watcher->running()) {
      if (ImGui::Button(ICON_FA_PLAY " start")) {
        watcher_init(watcher, config->local_dir);
        app->watcher_log.push_back(config->local_dir +
                                   ": watching for changes");
      }
    } else {
      if (ImGui::Button(ICON_FA_STOP " stop")) {
        watcher_destroy(watcher);
        app->watcher_log.push_back("stopped file watcher");
      }
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_BAN " clear log")) {
      app->watcher_log.clear();
    }

    if (ImGui::BeginChild("watcher log", ImGui::GetContentRegionAvail())) {
      for (auto &line : app->watcher_log) {
        ImGui::TextUnformatted(line.data());
      }

      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();

  watcher_poll(watcher);
  for (auto &change : watcher->changes) {
    switch (change.type) {
    case FILE_ACTION_MODIFIED:
      auto local = config->local_dir / fs::path(change.filename);
      if (fs::is_regular_file(local)) {
        i64 modified = fs::last_write_time(local).time_since_epoch().count();
        if (watcher->modtimes[change.filename] < modified) {
          watcher->modtimes[change.filename] = modified;
          app->watcher_log.push_back(change.filename + ": modified");
          upload_file(config, net, change.filename);
        }
      }
      break;
    }
  }
}

#if 0
#pragma comment(linker, "/subsystem:console")
int main(int, char **)
#else
#pragma comment(linker, "/subsystem:windows")
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int)
#endif
{
  if (!glfwInit()) {
    exit(1);
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  auto window = glfwCreateWindow(800, 600, "file-sink", nullptr, nullptr);
  if (!window) {
    exit(1);
  }

  glfwMakeContextCurrent(window);
  gladLoadGL((GLADloadfunc)glfwGetProcAddress);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 130");

  io.Fonts->AddFontFromFileTTF("data/Roboto-Regular.ttf", 16.0f);

  ImFontConfig font_config;
  font_config.MergeMode = true;
  font_config.GlyphMinAdvanceX = 14.0f;
  const ImWchar icon_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
  io.Fonts->AddFontFromFileTTF("data/fontawesome-webfont.ttf", 14.0f,
                               &font_config, icon_ranges);

  ImVec4 *colors = ImGui::GetStyle().Colors;
  colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.19f, 0.19f, 0.19f, 0.92f);
  colors[ImGuiCol_Border] = ImVec4(0.19f, 0.19f, 0.19f, 0.29f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.19f, 0.54f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 0.54f);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.54f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.33f, 0.67f, 0.86f, 1.00f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.54f);
  colors[ImGuiCol_Button] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.19f, 0.19f, 0.54f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
  colors[ImGuiCol_Header] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.00f, 0.00f, 0.36f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.22f, 0.23f, 0.33f);
  colors[ImGuiCol_Separator] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
  colors[ImGuiCol_SeparatorHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
  colors[ImGuiCol_SeparatorActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
  colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
  colors[ImGuiCol_Tab] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
  colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.20f, 0.20f, 0.36f);
  colors[ImGuiCol_TabUnfocused] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
  colors[ImGuiCol_DockingPreview] = ImVec4(0.33f, 0.67f, 0.86f, 1.00f);
  colors[ImGuiCol_DockingEmptyBg] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_TableHeaderBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
  colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
  colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
  colors[ImGuiCol_DragDropTarget] = ImVec4(0.33f, 0.67f, 0.86f, 1.00f);
  colors[ImGuiCol_NavHighlight] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
  colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 0.00f, 0.00f, 0.70f);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(1.00f, 0.00f, 0.00f, 0.20f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.35f);

  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(8.00f, 8.00f);
  style.FramePadding = ImVec2(5.00f, 2.00f);
  style.CellPadding = ImVec2(6.00f, 6.00f);
  style.ItemSpacing = ImVec2(6.00f, 6.00f);
  style.ItemInnerSpacing = ImVec2(6.00f, 6.00f);
  style.TouchExtraPadding = ImVec2(0.00f, 0.00f);
  style.IndentSpacing = 25;
  style.ScrollbarSize = 15;
  style.GrabMinSize = 10;
  style.WindowBorderSize = 1;
  style.ChildBorderSize = 1;
  style.PopupBorderSize = 1;
  style.FrameBorderSize = 1;
  style.TabBorderSize = 1;
  style.WindowRounding = 7;
  style.ChildRounding = 4;
  style.FrameRounding = 3;
  style.PopupRounding = 4;
  style.ScrollbarRounding = 9;
  style.GrabRounding = 3;
  style.LogSliderDeadzone = 4;
  style.TabRounding = 4;

  WSADATA wsadata;
  i32 wsa_error = WSAStartup(MAKEWORD(2, 0), &wsadata);
  if (wsa_error) {
    exit(1);
  }

  i32 ssh2_error = libssh2_init(0);
  if (ssh2_error) {
    exit(1);
  }

  App app;
  app.window = window;

  Config config;
  read_config(&config);

  Net net;

  FileWatcher watcher;

  while (!glfwWindowShouldClose(window)) {
    glfwWaitEventsTimeout(0.25);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    app_update(&app, &config, &net, &watcher);

    ImGui::Render();
    i32 width, height;
    glfwGetFramebufferSize(window, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      auto backup_current_context = glfwGetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      glfwMakeContextCurrent(backup_current_context);
    }

    glfwSwapBuffers(window);
  }

  if (net.session) {
    server_disconnect(&net);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  printf("bye\n");
}