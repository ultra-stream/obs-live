#include "browser-panel-internal.hpp"
#include "browser-panel-client.hpp"
#include "cef-headers.hpp"
#include "browser-app.hpp"

#include <QWindow>
#include <QApplication>

#ifdef USE_QT_LOOP
#include <QEventLoop>
#include <QThread>
#endif

#include <obs-module.h>
#include <util/threading.h>
#include <util/base.h>
#include <thread>

extern bool QueueCEFTask(std::function<void()> task);
extern "C" void obs_browser_initialize(void);
extern os_event_t *cef_started_event;

std::mutex popup_whitelist_mutex;
std::vector<PopupWhitelistInfo> popup_whitelist;
std::vector<PopupWhitelistInfo> forced_popups;

/* ------------------------------------------------------------------------- */

#if CHROME_VERSION_BUILD < 3770
CefRefPtr<CefCookieManager> QCefRequestContextHandler::GetCookieManager()
{
	return cm;
}
#endif

class CookieCheck : public CefCookieVisitor {
public:
	QCefCookieManager::cookie_exists_cb callback;
	std::string target;
	bool cookie_found = false;

	inline CookieCheck(QCefCookieManager::cookie_exists_cb callback_,
			   const std::string target_)
		: callback(callback_), target(target_)
	{
	}

	virtual ~CookieCheck() { callback(cookie_found); }

	virtual bool Visit(const CefCookie &cookie, int, int, bool &) override
	{
		CefString cef_name = cookie.name.str;
		std::string name = cef_name;

		if (name == target) {
			cookie_found = true;
			return false;
		}
		return true;
	}

	IMPLEMENT_REFCOUNTING(CookieCheck);
};

struct QCefCookieManagerInternal : QCefCookieManager {
	CefRefPtr<CefCookieManager> cm;
#if CHROME_VERSION_BUILD < 3770
	CefRefPtr<CefRequestContextHandler> rch;
#endif
	CefRefPtr<CefRequestContext> rc;

	QCefCookieManagerInternal(const std::string &storage_path,
				  bool persist_session_cookies)
	{
		if (os_event_try(cef_started_event) != 0)
			throw "Browser thread not initialized";

		BPtr<char> path = obs_module_config_path(storage_path.c_str());

#if CHROME_VERSION_BUILD < 3770
		cm = CefCookieManager::CreateManager(
			path.Get(), persist_session_cookies, nullptr);
		if (!cm)
			throw "Failed to create cookie manager";
#endif

#if CHROME_VERSION_BUILD < 3770
		rch = new QCefRequestContextHandler(cm);

		rc = CefRequestContext::CreateContext(
			CefRequestContext::GetGlobalContext(), rch);
#else
		CefRequestContextSettings settings;
		CefString(&settings.cache_path) = path.Get();
		rc = CefRequestContext::CreateContext(
			settings, CefRefPtr<CefRequestContextHandler>());
		if (rc)
			cm = rc->GetCookieManager(nullptr);

		UNUSED_PARAMETER(persist_session_cookies);
#endif
	}

	virtual bool DeleteCookies(const std::string &url,
				   const std::string &name) override
	{
		return !!cm ? cm->DeleteCookies(url, name, nullptr) : false;
	}

	virtual bool SetStoragePath(const std::string &storage_path,
				    bool persist_session_cookies) override
	{
		BPtr<char> path = obs_module_config_path(storage_path.c_str());

#if CHROME_VERSION_BUILD < 3770
		return cm->SetStoragePath(path.Get(), persist_session_cookies,
					  nullptr);
#else
		CefRequestContextSettings settings;
		CefString(&settings.cache_path) = storage_path;
		rc = CefRequestContext::CreateContext(
			settings, CefRefPtr<CefRequestContextHandler>());
		if (rc)
			cm = rc->GetCookieManager(nullptr);

		UNUSED_PARAMETER(persist_session_cookies);
		return true;
#endif
	}

	virtual bool FlushStore() override
	{
		return !!cm ? cm->FlushStore(nullptr) : false;
	}

	virtual void CheckForCookie(const std::string &site,
				    const std::string &cookie,
				    cookie_exists_cb callback) override
	{
		if (!cm)
			return;

		CefRefPtr<CookieCheck> c = new CookieCheck(callback, cookie);
		cm->VisitUrlCookies(site, false, c);
	}
};

/* ------------------------------------------------------------------------- */

QCefWidgetInternal::QCefWidgetInternal(QWidget *parent, const std::string &url_,
				       CefRefPtr<CefRequestContext> rqc_)
	: QCefWidget(parent), url(url_), rqc(rqc_)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	setFocusPolicy(Qt::ClickFocus);
}

QCefWidgetInternal::~QCefWidgetInternal()
{
	CefRefPtr<CefBrowser> browser = cefBrowser;
	if (!!browser) {
		auto destroyBrowser = [](CefRefPtr<CefBrowser> cefBrowser) {
			CefRefPtr<CefClient> client =
				cefBrowser->GetHost()->GetClient();
			QCefBrowserClient *bc =
				reinterpret_cast<QCefBrowserClient *>(
					client.get());

			cefBrowser->GetHost()->WasHidden(true);
#ifdef _WIN32
			/* So you're probably wondering what's going on here.
			 * If you call CefBrowserHost::CloseBrowser, and it
			 * fails to unload the web page *before* WM_NCDESTROY
			 * is called on the browser HWND, it will call an
			 * internal CEF function
			 * CefBrowserPlatformDelegateNativeWin::CloseHostWindow,
			 * which will attempt to close the browser's main
			 * window itself.  Problem is, this closes the root
			 * window containing the browser's HWND rather than the
			 * browser's specific HWND for whatever mysterious
			 * reason.  If the browser is in a dock widget, then
			 * the window it closes is, unfortunately, the main
			 * program's window, causing the entire program to shut
			 * down.
			 *
			 * So, instead, we want to destroy the browser by
			 * calling DestroyWindow on the browser's widget
			 * ourselves to ensure that WM_NCDESTROY is called.
			 * This will also forcibly destroy the browser, so
			 * calling CloseBrowser(true) is unnecessary. */
			HWND hwnd = cefBrowser->GetHost()->GetWindowHandle();
			if (hwnd)
				DestroyWindow(hwnd);
#else
			cefBrowser->GetHost()->CloseBrowser(true);
#endif

			bc->widget = nullptr;
		};

#ifdef USE_QT_LOOP
		destroyBrowser(browser);
#else
		os_event_t *finishedEvent;
		os_event_init(&finishedEvent, OS_EVENT_TYPE_AUTO);
		bool success = QueueCEFTask([=]() {
			destroyBrowser(browser);
			os_event_signal(finishedEvent);
		});
		if (success) {
			/* fixes an issue on windows where blocking the main
			 * UI thread can cause CEF SendMessage calls calls
			 * to lock up */
			int code = ETIMEDOUT;
			while (code == ETIMEDOUT) {
				QCoreApplication::processEvents();
				code = os_event_timedwait(finishedEvent, 5);
			}
		}
		os_event_destroy(finishedEvent);
#endif

		cefBrowser = nullptr;
	}
}

void QCefWidgetInternal::Init()
{
	QSize size = this->size() * devicePixelRatio();
	WId id = winId();

	bool success = QueueCEFTask([this, size, id]() {
		CefWindowInfo windowInfo;

		/* Make sure Init isn't called more than once. */
		if (cefBrowser)
			return;

#ifdef _WIN32
		RECT rc = {0, 0, size.width(), size.height()};
		windowInfo.SetAsChild((HWND)id, rc);
#elif __APPLE__
		windowInfo.SetAsChild((CefWindowHandle)id, 0, 0, size.width(),
				      size.height());
#endif

		CefRefPtr<QCefBrowserClient> browserClient =
			new QCefBrowserClient(this, script, allowAllPopups_);

		CefBrowserSettings cefBrowserSettings;
		cefBrowser = CefBrowserHost::CreateBrowserSync(
			windowInfo, browserClient, url, cefBrowserSettings,
#if CHROME_VERSION_BUILD >= 3770
			CefRefPtr<CefDictionaryValue>(),
#endif
			rqc);
#ifdef _WIN32
		Resize();
#endif
	});

	if (success)
		timer.stop();
}

void QCefWidgetInternal::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	Resize();
}

void QCefWidgetInternal::Resize()
{
#ifdef _WIN32
	QSize size = this->size() * devicePixelRatio();

	QueueCEFTask([this, size]() {
		if (!cefBrowser)
			return;
		HWND hwnd = cefBrowser->GetHost()->GetWindowHandle();
		SetWindowPos(hwnd, nullptr, 0, 0, size.width(), size.height(),
			     SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOZORDER);
		SendMessage(hwnd, WM_SIZE, 0,
			    MAKELPARAM(size.width(), size.height()));
	});
#endif
}

void QCefWidgetInternal::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (!cefBrowser) {
		obs_browser_initialize();
		connect(&timer, SIGNAL(timeout()), this, SLOT(Init()));
		timer.start(500);
		Init();
	}
}

QPaintEngine *QCefWidgetInternal::paintEngine() const
{
	return nullptr;
}

void QCefWidgetInternal::setURL(const std::string &url)
{
	if (cefBrowser) {
		cefBrowser->GetMainFrame()->LoadURL(url);
	}
}

void QCefWidgetInternal::setStartupScript(const std::string &script_)
{
	script = script_;
}

void QCefWidgetInternal::allowAllPopups(bool allow)
{
	allowAllPopups_ = allow;
}

/* ------------------------------------------------------------------------- */

struct QCefInternal : QCef {
	virtual bool init_browser(void) override;
	virtual bool initialized(void) override;
	virtual bool wait_for_browser_init(void) override;

	virtual QCefWidget *
	create_widget(QWidget *parent, const std::string &url,
		      QCefCookieManager *cookie_manager) override;

	virtual QCefCookieManager *
	create_cookie_manager(const std::string &storage_path,
			      bool persist_session_cookies) override;

	virtual BPtr<char>
	get_cookie_path(const std::string &storage_path) override;

	virtual void add_popup_whitelist_url(const std::string &url,
					     QObject *obj) override;
	virtual void add_force_popup_url(const std::string &url,
					 QObject *obj) override;
};

bool QCefInternal::init_browser(void)
{
	if (os_event_try(cef_started_event) == 0)
		return true;

	obs_browser_initialize();
	return false;
}

bool QCefInternal::initialized(void)
{
	return os_event_try(cef_started_event) == 0;
}

bool QCefInternal::wait_for_browser_init(void)
{
	return os_event_wait(cef_started_event) == 0;
}

QCefWidget *QCefInternal::create_widget(QWidget *parent, const std::string &url,
					QCefCookieManager *cm)
{
	QCefCookieManagerInternal *cmi =
		reinterpret_cast<QCefCookieManagerInternal *>(cm);

	return new QCefWidgetInternal(parent, url, cmi ? cmi->rc : nullptr);
}

QCefCookieManager *
QCefInternal::create_cookie_manager(const std::string &storage_path,
				    bool persist_session_cookies)
{
	try {
		return new QCefCookieManagerInternal(storage_path,
						     persist_session_cookies);
	} catch (const char *error) {
		blog(LOG_ERROR, "Failed to create cookie manager: %s", error);
		return nullptr;
	}
}

BPtr<char> QCefInternal::get_cookie_path(const std::string &storage_path)
{
	return obs_module_config_path(storage_path.c_str());
}

void QCefInternal::add_popup_whitelist_url(const std::string &url, QObject *obj)
{
	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	popup_whitelist.emplace_back(url, obj);
}

void QCefInternal::add_force_popup_url(const std::string &url, QObject *obj)
{
	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	forced_popups.emplace_back(url, obj);
}

extern "C" EXPORT QCef *obs_browser_create_qcef(void)
{
	return new QCefInternal();
}

#define BROWSER_PANEL_VERSION 1

extern "C" EXPORT int obs_browser_qcef_version_export(void)
{
	return BROWSER_PANEL_VERSION;
}
