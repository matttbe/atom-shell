// Copyright (c) 2014 GitHub, Inc. All rights reserved.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/native_window_views.h"

#if defined(OS_WIN)
#include <shobjidl.h>
#endif

#include <string>
#include <vector>

#include "atom/browser/ui/views/menu_bar.h"
#include "atom/browser/ui/views/menu_layout.h"
#include "atom/common/draggable_region.h"
#include "atom/common/options_switches.h"
#include "base/strings/utf_string_conversions.h"
#include "browser/inspectable_web_contents_view.h"
#include "content/public/browser/native_web_keyboard_event.h"
#include "native_mate/dictionary.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/hit_test.h"
#include "ui/views/background.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/window/client_view.h"
#include "ui/views/widget/widget.h"

#if defined(USE_X11)
#include "atom/browser/browser.h"
#include "atom/browser/ui/views/global_menu_bar_x11.h"
#include "atom/browser/ui/views/frameless_view.h"
#include "atom/browser/ui/x/x_window_utils.h"
#include "base/environment.h"
#include "base/nix/xdg_util.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/ui/libgtk2ui/unity_service.h"
#include "ui/base/x/x11_util.h"
#include "ui/gfx/x/x11_types.h"
#include "ui/views/window/native_frame_view.h"
#elif defined(OS_WIN)
#include "atom/browser/ui/views/win_frame_view.h"
#include "base/win/scoped_comptr.h"
#include "base/win/windows_version.h"
#include "ui/base/win/shell.h"
#include "ui/views/win/hwnd_util.h"
#endif

namespace atom {

namespace {

// The menu bar height in pixels.
#if defined(OS_WIN)
const int kMenuBarHeight = 20;
#else
const int kMenuBarHeight = 25;
#endif

#if defined(USE_X11)
// Counts how many window has already been created, it will be used to set the
// window role for X11.
int kWindowsCreated = 0;

bool ShouldUseGlobalMenuBar() {
  // Some DE would pretend to be Unity but don't have global application menu,
  // so we can not trust unity::IsRunning().
  scoped_ptr<base::Environment> env(base::Environment::Create());
  return unity::IsRunning() && (base::nix::GetDesktopEnvironment(env.get()) ==
      base::nix::DESKTOP_ENVIRONMENT_UNITY);
}
#endif

bool IsAltKey(const content::NativeWebKeyboardEvent& event) {
#if defined(USE_X11)
  // 164 and 165 represent VK_LALT and VK_RALT.
  return event.windowsKeyCode == 164 || event.windowsKeyCode == 165;
#else
  return event.windowsKeyCode == ui::VKEY_MENU;
#endif
}

bool IsAltModifier(const content::NativeWebKeyboardEvent& event) {
  typedef content::NativeWebKeyboardEvent::Modifiers Modifiers;
  return (event.modifiers == Modifiers::AltKey) ||
         (event.modifiers == (Modifiers::AltKey | Modifiers::IsLeft)) ||
         (event.modifiers == (Modifiers::AltKey | Modifiers::IsRight));
}

class NativeWindowClientView : public views::ClientView {
 public:
  NativeWindowClientView(views::Widget* widget,
                         NativeWindowViews* contents_view)
      : views::ClientView(widget, contents_view) {
  }
  virtual ~NativeWindowClientView() {}

  virtual bool CanClose() OVERRIDE {
    static_cast<NativeWindowViews*>(contents_view())->CloseWebContents();
    return false;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(NativeWindowClientView);
};

}  // namespace

NativeWindowViews::NativeWindowViews(content::WebContents* web_contents,
                                     const mate::Dictionary& options)
    : NativeWindow(web_contents, options),
      window_(new views::Widget),
      web_view_(inspectable_web_contents()->GetView()->GetView()),
      menu_bar_autohide_(false),
      menu_bar_visible_(false),
      menu_bar_alt_pressed_(false),
      keyboard_event_handler_(new views::UnhandledKeyboardEventHandler),
      use_content_size_(false),
      resizable_(true) {
  options.Get(switches::kTitle, &title_);
  options.Get(switches::kAutoHideMenuBar, &menu_bar_autohide_);

#if defined(OS_WIN)
  // On Windows we rely on the CanResize() to indicate whether window can be
  // resized, and it should be set before window is created.
  options.Get(switches::kResizable, &resizable_);
#endif

  if (enable_larger_than_screen_)
    // We need to set a default maximum window size here otherwise Windows
    // will not allow us to resize the window larger than scree.
    // Setting directly to INT_MAX somehow doesn't work, so we just devide
    // by 10, which should still be large enough.
    maximum_size_.SetSize(INT_MAX / 10, INT_MAX / 10);

  int width = 800, height = 600;
  options.Get(switches::kWidth, &width);
  options.Get(switches::kHeight, &height);
  gfx::Rect bounds(0, 0, width, height);

  window_->AddObserver(this);

  views::Widget::InitParams params;
  params.ownership = views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;
  params.bounds = bounds;
  params.delegate = this;
  params.type = views::Widget::InitParams::TYPE_WINDOW;
  params.remove_standard_frame = !has_frame_;

#if defined(USE_X11)
  // Set WM_WINDOW_ROLE.
  params.wm_role_name = base::StringPrintf(
      "%s/%s/%d", "Atom Shell", Browser::Get()->GetName().c_str(),
      ++kWindowsCreated);
  // Set WM_CLASS.
  params.wm_class_name = "atom";
  params.wm_class_class = "Atom";
#endif

  window_->Init(params);

#if defined(USE_X11)
  // Set _GTK_THEME_VARIANT to dark if we have "dark-theme" option set.
  bool use_dark_theme = false;
  if (options.Get(switches::kDarkTheme, &use_dark_theme) && use_dark_theme) {
    XDisplay* xdisplay = gfx::GetXDisplay();
    XChangeProperty(xdisplay, GetAcceleratedWidget(),
                    XInternAtom(xdisplay, "_GTK_THEME_VARIANT", False),
                    XInternAtom(xdisplay, "UTF8_STRING", False),
                    8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>("dark"),
                    4);
  }

  // Before the window is mapped the SetWMSpecState can not work, so we have
  // to manually set the _NET_WM_STATE.
  bool skip_taskbar = false;
  if (options.Get(switches::kSkipTaskbar, &skip_taskbar) && skip_taskbar) {
    std::vector<::Atom> state_atom_list;
    state_atom_list.push_back(GetAtom("_NET_WM_STATE_SKIP_TASKBAR"));
    ui::SetAtomArrayProperty(GetAcceleratedWidget(), "_NET_WM_STATE", "ATOM",
                             state_atom_list);
  }
#endif

  // Add web view.
  SetLayoutManager(new MenuLayout(kMenuBarHeight));
  set_background(views::Background::CreateStandardPanelBackground());
  AddChildView(web_view_);

  if (has_frame_ &&
      options.Get(switches::kUseContentSize, &use_content_size_) &&
      use_content_size_)
    bounds = ContentBoundsToWindowBounds(bounds);

  window_->UpdateWindowIcon();
  window_->CenterWindow(bounds.size());
  Layout();
}

NativeWindowViews::~NativeWindowViews() {
  window_->RemoveObserver(this);
}

void NativeWindowViews::Close() {
  window_->Close();
}

void NativeWindowViews::CloseImmediately() {
  window_->CloseNow();
}

void NativeWindowViews::Move(const gfx::Rect& bounds) {
  window_->SetBounds(bounds);
}

void NativeWindowViews::Focus(bool focus) {
  if (focus)
    window_->Activate();
  else
    window_->Deactivate();
}

bool NativeWindowViews::IsFocused() {
  return window_->IsActive();
}

void NativeWindowViews::Show() {
  window_->ShowInactive();
}

void NativeWindowViews::Hide() {
  window_->Hide();
}

bool NativeWindowViews::IsVisible() {
  return window_->IsVisible();
}

void NativeWindowViews::Maximize() {
  window_->Maximize();
}

void NativeWindowViews::Unmaximize() {
  window_->Restore();
}

bool NativeWindowViews::IsMaximized() {
  return window_->IsMaximized();
}

void NativeWindowViews::Minimize() {
  window_->Minimize();
}

void NativeWindowViews::Restore() {
  window_->Restore();
}

bool NativeWindowViews::IsMinimized() {
  return window_->IsMinimized();
}

void NativeWindowViews::SetFullscreen(bool fullscreen) {
  window_->SetFullscreen(fullscreen);
}

bool NativeWindowViews::IsFullscreen() {
  return window_->IsFullscreen();
}

void NativeWindowViews::SetSize(const gfx::Size& size) {
#if defined(USE_X11)
  // On Linux the minimum and maximum size should be updated with window size
  // when window is not resizable.
  if (!resizable_) {
    SetMaximumSize(size);
    SetMinimumSize(size);
  }
#endif

  window_->SetSize(size);
}

gfx::Size NativeWindowViews::GetSize() {
#if defined(OS_WIN)
  if (IsMinimized())
    return window_->GetRestoredBounds().size();
#endif

  return window_->GetWindowBoundsInScreen().size();
}

void NativeWindowViews::SetContentSize(const gfx::Size& size) {
  if (!has_frame_) {
    SetSize(size);
    return;
  }

  gfx::Rect bounds = window_->GetWindowBoundsInScreen();
  SetSize(ContentBoundsToWindowBounds(gfx::Rect(bounds.origin(), size)).size());
}

gfx::Size NativeWindowViews::GetContentSize() {
  if (!has_frame_)
    return GetSize();

  gfx::Size content_size =
      window_->non_client_view()->frame_view()->GetBoundsForClientView().size();
  if (menu_bar_ && menu_bar_visible_)
    content_size.set_height(content_size.height() - kMenuBarHeight);
  return content_size;
}

void NativeWindowViews::SetMinimumSize(const gfx::Size& size) {
  minimum_size_ = size;

#if defined(USE_X11)
  XSizeHints size_hints;
  size_hints.flags = PMinSize;
  size_hints.min_width = size.width();
  size_hints.min_height = size.height();
  XSetWMNormalHints(gfx::GetXDisplay(), GetAcceleratedWidget(), &size_hints);
#endif
}

gfx::Size NativeWindowViews::GetMinimumSize() {
  return minimum_size_;
}

void NativeWindowViews::SetMaximumSize(const gfx::Size& size) {
  maximum_size_ = size;

#if defined(USE_X11)
  XSizeHints size_hints;
  size_hints.flags = PMaxSize;
  size_hints.max_width = size.width();
  size_hints.max_height = size.height();
  XSetWMNormalHints(gfx::GetXDisplay(), GetAcceleratedWidget(), &size_hints);
#endif
}

gfx::Size NativeWindowViews::GetMaximumSize() {
  return maximum_size_;
}

void NativeWindowViews::SetResizable(bool resizable) {
#if defined(OS_WIN)
  if (has_frame_) {
    // WS_MAXIMIZEBOX => Maximize button
    // WS_MINIMIZEBOX => Minimize button
    // WS_THICKFRAME => Resize handle
    DWORD style = ::GetWindowLong(GetAcceleratedWidget(), GWL_STYLE);
    if (resizable)
      style |= WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_THICKFRAME;
    else
      style = (style & ~(WS_MAXIMIZEBOX | WS_THICKFRAME)) | WS_MINIMIZEBOX;
    ::SetWindowLong(GetAcceleratedWidget(), GWL_STYLE, style);
  }
#elif defined(USE_X11)
  if (resizable != resizable_) {
    // On Linux there is no "resizable" property of a window, we have to set
    // both the minimum and maximum size to the window size to achieve it.
    if (resizable) {
      SetMaximumSize(gfx::Size());
      SetMinimumSize(gfx::Size());
    } else {
      SetMaximumSize(GetSize());
      SetMinimumSize(GetSize());
    }
  }
#endif

  resizable_ = resizable;
}

bool NativeWindowViews::IsResizable() {
  return resizable_;
}

void NativeWindowViews::SetAlwaysOnTop(bool top) {
  window_->SetAlwaysOnTop(top);
}

bool NativeWindowViews::IsAlwaysOnTop() {
  return window_->IsAlwaysOnTop();
}

void NativeWindowViews::Center() {
  window_->CenterWindow(GetSize());
}

void NativeWindowViews::SetPosition(const gfx::Point& position) {
  window_->SetBounds(gfx::Rect(position, GetSize()));
}

gfx::Point NativeWindowViews::GetPosition() {
#if defined(OS_WIN)
  if (IsMinimized())
    return window_->GetRestoredBounds().origin();
#endif

  return window_->GetWindowBoundsInScreen().origin();
}

void NativeWindowViews::SetTitle(const std::string& title) {
  title_ = title;
  window_->UpdateWindowTitle();
}

std::string NativeWindowViews::GetTitle() {
  return title_;
}

void NativeWindowViews::FlashFrame(bool flash) {
  window_->FlashFrame(flash);
}

void NativeWindowViews::SetSkipTaskbar(bool skip) {
#if defined(OS_WIN)
  base::win::ScopedComPtr<ITaskbarList> taskbar;
  if (FAILED(taskbar.CreateInstance(CLSID_TaskbarList, NULL,
                                    CLSCTX_INPROC_SERVER)) ||
      FAILED(taskbar->HrInit()))
    return;
  if (skip)
    taskbar->DeleteTab(GetAcceleratedWidget());
  else
    taskbar->AddTab(GetAcceleratedWidget());
#elif defined(USE_X11)
  SetWMSpecState(GetAcceleratedWidget(), skip,
                 GetAtom("_NET_WM_STATE_SKIP_TASKBAR"));
#endif
}

void NativeWindowViews::SetKiosk(bool kiosk) {
  SetFullscreen(kiosk);
}

bool NativeWindowViews::IsKiosk() {
  return IsFullscreen();
}

void NativeWindowViews::SetMenu(ui::MenuModel* menu_model) {
  RegisterAccelerators(menu_model);

#if defined(USE_X11)
  if (!global_menu_bar_ && ShouldUseGlobalMenuBar())
    global_menu_bar_.reset(new GlobalMenuBarX11(this));

  // Use global application menu bar when possible.
  if (global_menu_bar_ && global_menu_bar_->IsServerStarted()) {
    global_menu_bar_->SetMenu(menu_model);
    return;
  }
#endif

  // Do not show menu bar in frameless window.
  if (!has_frame_)
    return;

  if (!menu_bar_) {
    gfx::Size content_size = GetContentSize();
    menu_bar_.reset(new MenuBar);
    menu_bar_->set_owned_by_client();

    if (!menu_bar_autohide_) {
      SetMenuBarVisibility(true);
      if (use_content_size_)
        SetContentSize(content_size);
    }
  }

  menu_bar_->SetMenu(menu_model);
  Layout();
}

gfx::NativeWindow NativeWindowViews::GetNativeWindow() {
  return window_->GetNativeWindow();
}

void NativeWindowViews::SetProgressBar(double progress) {
#if defined(OS_WIN)
  if (base::win::GetVersion() < base::win::VERSION_WIN7)
    return;
  base::win::ScopedComPtr<ITaskbarList3> taskbar;
  if (FAILED(taskbar.CreateInstance(CLSID_TaskbarList, NULL,
                                    CLSCTX_INPROC_SERVER) ||
      FAILED(taskbar->HrInit()))) {
    return;
  }
  HWND frame = views::HWNDForNativeWindow(GetNativeWindow());
  if (progress > 1.0) {
    taskbar->SetProgressState(frame, TBPF_INDETERMINATE);
  } else if (progress < 0) {
    taskbar->SetProgressState(frame, TBPF_NOPROGRESS);
  } else if (progress >= 0) {
    taskbar->SetProgressValue(frame,
                              static_cast<int>(progress * 100),
                              100);
  }
#elif defined(USE_X11)
  if (unity::IsRunning()) {
    unity::SetProgressFraction(progress);
  }
#endif
}

gfx::AcceleratedWidget NativeWindowViews::GetAcceleratedWidget() {
  return GetNativeWindow()->GetHost()->GetAcceleratedWidget();
}

void NativeWindowViews::UpdateDraggableRegions(
    const std::vector<DraggableRegion>& regions) {
  if (has_frame_)
    return;

  SkRegion* draggable_region = new SkRegion;

  // By default, the whole window is non-draggable. We need to explicitly
  // include those draggable regions.
  for (std::vector<DraggableRegion>::const_iterator iter = regions.begin();
       iter != regions.end(); ++iter) {
    const DraggableRegion& region = *iter;
    draggable_region->op(
        region.bounds.x(),
        region.bounds.y(),
        region.bounds.right(),
        region.bounds.bottom(),
        region.draggable ? SkRegion::kUnion_Op : SkRegion::kDifference_Op);
  }

  draggable_region_.reset(draggable_region);
}

void NativeWindowViews::OnWidgetActivationChanged(
    views::Widget* widget, bool active) {
  if (widget != window_.get())
    return;

  if (active)
    NotifyWindowFocus();
  else
    NotifyWindowBlur();

  if (active && GetWebContents() && !IsDevToolsOpened())
    GetWebContents()->Focus();

  // Hide menu bar when window is blured.
  if (!active && menu_bar_autohide_ && menu_bar_visible_) {
    SetMenuBarVisibility(false);
    Layout();
  }
}

void NativeWindowViews::DeleteDelegate() {
  NotifyWindowClosed();
}

views::View* NativeWindowViews::GetInitiallyFocusedView() {
  return inspectable_web_contents()->GetView()->GetWebView();
}

bool NativeWindowViews::CanResize() const {
  return resizable_;
}

bool NativeWindowViews::CanMaximize() const {
  return resizable_;
}

base::string16 NativeWindowViews::GetWindowTitle() const {
  return base::UTF8ToUTF16(title_);
}

bool NativeWindowViews::ShouldHandleSystemCommands() const {
  return true;
}

gfx::ImageSkia NativeWindowViews::GetWindowAppIcon() {
  return icon_;
}

gfx::ImageSkia NativeWindowViews::GetWindowIcon() {
  return GetWindowAppIcon();
}

views::Widget* NativeWindowViews::GetWidget() {
  return window_.get();
}

const views::Widget* NativeWindowViews::GetWidget() const {
  return window_.get();
}

views::View* NativeWindowViews::GetContentsView() {
  return this;
}

bool NativeWindowViews::ShouldDescendIntoChildForEventHandling(
    gfx::NativeView child,
    const gfx::Point& location) {
  // App window should claim mouse events that fall within the draggable region.
  if (draggable_region_ &&
      draggable_region_->contains(location.x(), location.y()))
    return false;

  // And the events on border for dragging resizable frameless window.
  if (!has_frame_ && CanResize()) {
    FramelessView* frame = static_cast<FramelessView*>(
        window_->non_client_view()->frame_view());
    return frame->ResizingBorderHitTest(location) == HTNOWHERE;
  }

  return true;
}

views::ClientView* NativeWindowViews::CreateClientView(views::Widget* widget) {
  return new NativeWindowClientView(widget, this);
}

views::NonClientFrameView* NativeWindowViews::CreateNonClientFrameView(
    views::Widget* widget) {
#if defined(OS_WIN)
  if (ui::win::IsAeroGlassEnabled()) {
    WinFrameView* frame_view =  new WinFrameView;
    frame_view->Init(this, widget);
    return frame_view;
  }
#elif defined(OS_LINUX)
  if (has_frame_) {
    return new views::NativeFrameView(widget);
  } else {
    FramelessView* frame_view =  new FramelessView;
    frame_view->Init(this, widget);
    return frame_view;
  }
#endif

  return NULL;
}

gfx::ImageSkia NativeWindowViews::GetDevToolsWindowIcon() {
  return GetWindowAppIcon();
}

void NativeWindowViews::HandleMouseDown() {
  // Hide menu bar when web view is clicked.
  if (menu_bar_autohide_ && menu_bar_visible_) {
    SetMenuBarVisibility(false);
    Layout();
  }
}

void NativeWindowViews::HandleKeyboardEvent(
    content::WebContents*,
    const content::NativeWebKeyboardEvent& event) {
  keyboard_event_handler_->HandleKeyboardEvent(event, GetFocusManager());

  if (!menu_bar_)
    return;

  // Show accelerator when "Alt" is pressed.
  if (menu_bar_visible_ && IsAltKey(event))
    menu_bar_->SetAcceleratorVisibility(
        event.type == blink::WebInputEvent::RawKeyDown);

  // Show the submenu when "Alt+Key" is pressed.
  if (event.type == blink::WebInputEvent::RawKeyDown && !IsAltKey(event) &&
      IsAltModifier(event)) {
    if (!menu_bar_visible_ &&
        (menu_bar_->GetAcceleratorIndex(event.windowsKeyCode) != -1)) {
      SetMenuBarVisibility(true);
      Layout();
    }
    menu_bar_->ActivateAccelerator(event.windowsKeyCode);
    return;
  }

  if (!menu_bar_autohide_)
    return;

  // Toggle the menu bar only when a single Alt is released.
  if (event.type == blink::WebInputEvent::RawKeyDown && IsAltKey(event) &&
      IsAltModifier(event)) {
    // When a single Alt is pressed:
    menu_bar_alt_pressed_ = true;
  } else if (event.type == blink::WebInputEvent::KeyUp && IsAltKey(event) &&
             event.modifiers == 0 && menu_bar_alt_pressed_) {
    // When a single Alt is released right after a Alt is pressed:
    menu_bar_alt_pressed_ = false;
    SetMenuBarVisibility(!menu_bar_visible_);
    Layout();
  } else {
    // When any other keys except single Alt have been pressed/released:
    menu_bar_alt_pressed_ = false;
  }
}

bool NativeWindowViews::AcceleratorPressed(const ui::Accelerator& accelerator) {
  return accelerator_util::TriggerAcceleratorTableCommand(
      &accelerator_table_, accelerator);
}

void NativeWindowViews::RegisterAccelerators(ui::MenuModel* menu_model) {
  // Clear previous accelerators.
  views::FocusManager* focus_manager = GetFocusManager();
  accelerator_table_.clear();
  focus_manager->UnregisterAccelerators(this);

  // Register accelerators with focus manager.
  accelerator_util::GenerateAcceleratorTable(&accelerator_table_, menu_model);
  accelerator_util::AcceleratorTable::const_iterator iter;
  for (iter = accelerator_table_.begin();
       iter != accelerator_table_.end();
       ++iter) {
    focus_manager->RegisterAccelerator(
        iter->first, ui::AcceleratorManager::kNormalPriority, this);
  }
}

gfx::Rect NativeWindowViews::ContentBoundsToWindowBounds(
    const gfx::Rect& bounds) {
  gfx::Rect window_bounds =
      window_->non_client_view()->GetWindowBoundsForClientBounds(bounds);
  if (menu_bar_ && menu_bar_visible_)
    window_bounds.set_height(window_bounds.height() + kMenuBarHeight);
  return window_bounds;
}

void NativeWindowViews::SetMenuBarVisibility(bool visible) {
  if (!menu_bar_)
    return;

  // Always show the accelerator when the auto-hide menu bar shows.
  if (menu_bar_autohide_)
    menu_bar_->SetAcceleratorVisibility(visible);

  menu_bar_visible_ = visible;
  if (visible) {
    DCHECK_EQ(child_count(), 1);
    AddChildView(menu_bar_.get());
  } else {
    DCHECK_EQ(child_count(), 2);
    RemoveChildView(menu_bar_.get());
  }
}

// static
NativeWindow* NativeWindow::Create(content::WebContents* web_contents,
                                   const mate::Dictionary& options) {
  return new NativeWindowViews(web_contents, options);
}

}  // namespace atom
