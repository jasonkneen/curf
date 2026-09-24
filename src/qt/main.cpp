#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QSslSocket>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWebEngineCertificateError>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>

#include "../control_server.hpp"
#include "../inject.js.hpp"
#include "../url_util.hpp"

namespace {

QString Q(const std::string& s) { return QString::fromStdString(s); }

QString jsString(const QString& s) {
    QString a = QString::fromUtf8(QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact));
    return a.mid(1, a.size() - 2);
}

std::string toJSON(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString(); }

QString dataDir() {
    QString dir = QDir::homePath() + "/.curf";
    QDir().mkpath(dir);
    return dir;
}

void appendLine(const QString& path, const QJsonObject& o) {
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
}

QString isoNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }

// ---- Icons: drawn with QPainter so every platform gets the same crisp, tintable set. ----

enum class Glyph { Back, Forward, Reload, Stop, Lock, LockOpen, Warning, Info, Picker };

QIcon makeIcon(Glyph g, const QColor& color) {
    const qreal dpr = 3.0;
    QPixmap pm(QSize(24, 24) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    auto lockBody = [&] {
        p.setBrush(color);
        p.drawRoundedRect(QRectF(5.5, 11, 13, 9), 2, 2);
        p.setBrush(Qt::NoBrush);
    };
    switch (g) {
        case Glyph::Back: p.drawPolyline(QPolygonF{{15, 5}, {8, 12}, {15, 19}}); break;
        case Glyph::Forward: p.drawPolyline(QPolygonF{{9, 5}, {16, 12}, {9, 19}}); break;
        case Glyph::Reload: {
            QPainterPath path;
            QRectF r(5, 5, 14, 14);
            path.arcMoveTo(r, 60);
            path.arcTo(r, 60, 290);
            p.drawPath(path);
            p.setBrush(color);
            p.setPen(Qt::NoPen);
            p.drawPolygon(QPolygonF{{12.5, 2.5}, {17.5, 5.5}, {12.5, 9}});
            break;
        }
        case Glyph::Stop:
            p.drawLine(QPointF(6, 6), QPointF(18, 18));
            p.drawLine(QPointF(18, 6), QPointF(6, 18));
            break;
        case Glyph::Lock: {
            QPainterPath s;
            s.moveTo(8.5, 11);
            s.lineTo(8.5, 8);
            s.arcTo(QRectF(8.5, 4.5, 7, 7), 180, -180);
            s.lineTo(15.5, 11);
            p.drawPath(s);
            lockBody();
            break;
        }
        case Glyph::LockOpen: {
            QPainterPath s;
            s.moveTo(8.5, 11);
            s.lineTo(8.5, 6.5);
            s.arcTo(QRectF(8.5, 3, 7, 7), 180, -180);
            s.lineTo(15.5, 7.5);
            p.drawPath(s);
            lockBody();
            break;
        }
        case Glyph::Warning:
            p.drawPolygon(QPolygonF{{12, 3.5}, {21, 19.5}, {3, 19.5}});
            p.drawLine(QPointF(12, 9), QPointF(12, 13.5));
            p.drawPoint(QPointF(12, 16.8));
            break;
        case Glyph::Info:
            p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
            p.drawLine(QPointF(12, 11), QPointF(12, 16.5));
            p.drawPoint(QPointF(12, 7.8));
            break;
        case Glyph::Picker:
            p.setBrush(color);
            pen.setWidthF(1.2);
            p.setPen(pen);
            p.drawPolygon(QPolygonF{{5, 4}, {5, 18}, {9, 14.5}, {11.5, 20}, {13.5, 19}, {11, 13.5}, {16, 13.5}});
            pen.setWidthF(1.8);
            p.setPen(pen);
            p.drawLine(QPointF(15, 3), QPointF(15, 6));
            p.drawLine(QPointF(19.5, 5), QPointF(17.5, 7));
            p.drawLine(QPointF(21, 10), QPointF(18, 10));
            break;
    }
    return QIcon(pm);
}

// Flags mixed content: plain-HTTP subresources on an HTTPS page.
class MixedContentInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    std::atomic<bool> mainIsHttps{false};
    std::atomic<bool> mixed{false};
    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        if (mainIsHttps && info.resourceType() != QWebEngineUrlRequestInfo::ResourceTypeMainFrame &&
            info.requestUrl().scheme() == "http")
            mixed = true;
    }
};

class Page : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
    std::function<void(const QString&)> onCurfMessage;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString& message, int, const QString&) override {
        static const QString tag = "__curf__:";
        if (message.startsWith(tag) && onCurfMessage) onCurfMessage(message.mid(tag.size()));
    }
    QWebEnginePage* createWindow(WebWindowType) override { return this; }
};

}  // namespace

class Browser : public QMainWindow {
public:
    explicit Browser(int port) : port_(port) {
        resize(1280, 860);
        setWindowTitle("curf");

        profile_ = QWebEngineProfile::defaultProfile();
        profile_->setUrlRequestInterceptor(&interceptor_);
        page_ = new Page(profile_, this);
        view_ = new QWebEngineView(this);
        view_->setPage(page_);
        page_->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
        page_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);

        QWebEngineScript script;
        script.setName("curf");
        script.setSourceCode(QString::fromUtf8(kInjectedScript));
        script.setInjectionPoint(QWebEngineScript::DocumentReady);
        script.setWorldId(QWebEngineScript::MainWorld);
        script.setRunsOnSubFrames(false);
        page_->scripts().insert(script);

        buildToolbar();

        progress_ = new QProgressBar(this);
        progress_->setRange(0, 100);
        progress_->setTextVisible(false);
        progress_->setFixedHeight(2);
        progress_->setStyleSheet("QProgressBar{border:0;background:transparent}QProgressBar::chunk{background:#0a84ff}");
        progress_->hide();

        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(progress_);
        layout->addWidget(view_, 1);
        setCentralWidget(central);

        page_->onCurfMessage = [this](const QString& json) { handleMessage(json); };
        connect(page_, &QWebEnginePage::urlChanged, this, [this](const QUrl& u) {
            interceptor_.mainIsHttps = u.scheme() == "https";
            refreshChrome();
        });
        connect(page_, &QWebEnginePage::titleChanged, this, [this] { refreshChrome(); });
        connect(page_, &QWebEnginePage::loadStarted, this, [this] {
            loading_ = true;
            interceptor_.mixed = false;
            certError_.clear();
            record("navigation_started", {{"url", page_->requestedUrl().toString()}});
            refreshChrome();
        });
        connect(page_, &QWebEnginePage::loadProgress, this, [this](int p) {
            progress_->setValue(p);
        });
        connect(page_, &QWebEnginePage::loadFinished, this, [this](bool ok) {
            loading_ = false;
            if (ok) {
                record("navigation_finished",
                       {{"url", page_->url().toString()}, {"title", page_->title()}, {"secure", isSecure()}});
                if (pickAct_->isChecked()) setPicking(true);
            } else {
                record("navigation_failed", {{"url", page_->requestedUrl().toString()}});
            }
            refreshChrome();
        });
        connect(page_, &QWebEnginePage::certificateError, this, [this](const QWebEngineCertificateError& e) {
            certError_ = e.description();
            record("certificate_error", {{"url", e.url().toString()}, {"error", certError_}});
        });

        refreshChrome();
        startServer();
    }

    void navigate(const QString& input) {
        std::string resolved = curf::resolveInput(input.toStdString());
        if (resolved.empty()) return;
        QUrl url(Q(resolved));
        if (!url.isValid()) url = QUrl(Q("https://www.google.com/search?q=" + curf::urlEncode(input.toStdString())));
        view_->load(url);
        view_->setFocus();
    }

    void focusAddress() {
        address_->setFocus();
        address_->selectAll();
    }

private:
    // ---- UI ----
    QColor iconColor() const { return palette().color(QPalette::WindowText); }

    QAction* addButton(QToolBar* bar, Glyph g, const QString& tip, const QKeySequence& key,
                       std::function<void()> fn) {
        QAction* a = bar->addAction(makeIcon(g, iconColor()), tip);
        a->setToolTip(key.isEmpty() ? tip : tip + " (" + key.toString(QKeySequence::NativeText) + ")");
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, fn);
        return a;
    }

    void buildToolbar() {
        auto* bar = addToolBar("Navigation");
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setIconSize(QSize(18, 18));
        bar->setContextMenuPolicy(Qt::PreventContextMenu);
        bar->setStyleSheet("QToolBar{spacing:4px;padding:4px 6px}");

        backAct_ = addButton(bar, Glyph::Back, "Back", QKeySequence::Back, [this] { view_->back(); });
        fwdAct_ = addButton(bar, Glyph::Forward, "Forward", QKeySequence::Forward, [this] { view_->forward(); });
        reloadAct_ = addButton(bar, Glyph::Reload, "Reload", QKeySequence::Refresh, [this] {
            if (loading_) view_->stop();
            else view_->reload();
        });
        secureAct_ = addButton(bar, Glyph::Info, "Site information", QKeySequence("Ctrl+I"), [this] { showSiteInfo(); });

        address_ = new QLineEdit(bar);
        address_->setPlaceholderText("Search Google or type a URL");
        address_->setClearButtonEnabled(true);
        address_->setMinimumWidth(200);
        address_->setStyleSheet("QLineEdit{border-radius:6px;padding:4px 8px}");
        connect(address_, &QLineEdit::returnPressed, this, [this] { navigate(address_->text()); });
        bar->addWidget(address_);

        pickAct_ = addButton(bar, Glyph::Picker, "Select an element to annotate", QKeySequence("Ctrl+E"),
                             [this] { setPicking(pickAct_->isChecked()); });
        pickAct_->setCheckable(true);

        auto* focusUrl = new QAction(this);
        focusUrl->setShortcut(QKeySequence("Ctrl+L"));
        connect(focusUrl, &QAction::triggered, this, [this] { focusAddress(); });
        addAction(focusUrl);
    }

    bool isSecure() const { return page_->url().scheme() == "https" && !interceptor_.mixed && certError_.isEmpty(); }

    void refreshChrome() {
        const QUrl u = page_->url();
        if (!address_->hasFocus()) address_->setText(u.isEmpty() ? QString() : u.toString());
        setWindowTitle(page_->title().isEmpty() || page_->title() == u.toString() ? "curf" : page_->title() + " — curf");
        backAct_->setEnabled(page_->history()->canGoBack());
        fwdAct_->setEnabled(page_->history()->canGoForward());
        reloadAct_->setIcon(makeIcon(loading_ ? Glyph::Stop : Glyph::Reload, iconColor()));
        reloadAct_->setToolTip(loading_ ? "Stop" : "Reload");
        progress_->setVisible(loading_);

        Glyph g = Glyph::Info;
        QColor c = iconColor();
        QString tip = "Site information";
        if (u.scheme() == "https" && isSecure()) {
            g = Glyph::Lock; c = QColor("#30b94d"); tip = "Connection is secure";
        } else if (u.scheme() == "https") {
            g = Glyph::Warning; c = QColor("#f09a00");
            tip = certError_.isEmpty() ? "Secure connection, but page has insecure content" : "Certificate problem: " + certError_;
        } else if (u.scheme() == "http") {
            g = Glyph::LockOpen; c = QColor("#e5483a"); tip = "Connection is not secure";
        }
        secureAct_->setIcon(makeIcon(g, c));
        secureAct_->setToolTip(tip);
        secureTip_ = tip;
    }

    QString certificateSummary(const QUrl& u) {
        if (u.scheme() != "https") return "No certificate (connection is not encrypted).";
        if (!QSslSocket::supportsSsl()) return "Certificate details unavailable (no TLS backend).";
        QSslSocket sock;
        sock.setPeerVerifyMode(QSslSocket::VerifyPeer);
        sock.connectToHostEncrypted(u.host(), u.port(443));
        bool ok = sock.waitForEncrypted(5000);
        QStringList out;
        out << QString("Certificate: %1").arg(ok ? "valid and trusted" : "NOT verified (" + sock.errorString() + ")");
        const auto chain = sock.peerCertificateChain();
        for (int i = 0; i < chain.size(); ++i) {
            QString name = chain[i].subjectInfo(QSslCertificate::CommonName).join(", ");
            if (name.isEmpty()) name = chain[i].subjectInfo(QSslCertificate::Organization).join(", ");
            out << (i == 0 ? "Issued to: " : "  ↳ issued by: ") + name;
            if (i == 0) out << "Valid until: " + chain[i].expiryDate().toString("yyyy-MM-dd");
        }
        return out.join("\n");
    }

    void showSiteInfo() {
        const QUrl u = page_->url();
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QString cert = certificateSummary(u);
        QApplication::restoreOverrideCursor();
        QMessageBox box(this);
        box.setWindowTitle("Site information");
        box.setIconPixmap(secureAct_->icon().pixmap(48, 48));
        box.setText(u.host().isEmpty() ? "No site loaded" : u.host());
        box.setInformativeText(QString("%1\n\nURL: %2\nProtocol: %3\nMixed content: %4\n\n%5")
                                   .arg(secureTip_, u.toString(), u.scheme().toUpper(),
                                        interceptor_.mixed ? "yes" : "no", cert));
        box.exec();
    }

    void setPicking(bool on) {
        pickAct_->setChecked(on);
        page_->runJavaScript(QString("window.__curf && __curf.setActive(%1)").arg(on ? "true" : "false"));
        if (on) view_->setFocus();
    }

    // ---- Changes & annotations ----
    QJsonObject record(const QString& type, QJsonObject data) {
        {
            std::lock_guard<std::mutex> lock(changesMutex_);
            data["seq"] = static_cast<qint64>(++seq_);
            data["type"] = type;
            data["time"] = isoNow();
            changes_.push_back(data);
            if (changes_.size() > 10000) changes_.erase(changes_.begin());
        }
        appendLine(dataDir() + "/changes.jsonl", data);
        return data;
    }

    QJsonObject saveAnnotation(QJsonObject a, const QString& note) {
        a.remove("type");
        a["note"] = note;
        a["time"] = isoNow();
        if (!a.contains("url")) a["url"] = page_->url().toString();
        a["title"] = page_->title();
        appendLine(dataDir() + "/annotations.jsonl", a);
        record("annotation", a);
        page_->runJavaScript(
            QString("(()=>{const e=document.querySelector(%1);if(e){e.style.outline='2px dashed #ff2d95';e.title=%2;}})()")
                .arg(jsString(a["selector"].toString()), jsString(note)));
        return a;
    }

    void promptForElement(const QJsonObject& el) {
        QString text = el["text"].toString();
        if (text.size() > 240) text = text.left(240) + "…";
        bool ok = false;
        QString note = QInputDialog::getMultiLineText(
            this, "Annotate selected element",
            QString("<%1>  %2\n\n%3\n\nYour note / input:").arg(el["tag"].toString(), el["selector"].toString(), text),
            QString(), &ok);
        if (ok) saveAnnotation(el, note);
    }

    void handleMessage(const QString& json) {
        QJsonObject m = QJsonDocument::fromJson(json.toUtf8()).object();
        QString type = m["type"].toString();
        if (type == "evalResult") {
            std::shared_ptr<std::promise<QJsonObject>> p;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto it = pending_.find(m["id"].toInt());
                if (it == pending_.end()) return;
                p = it->second;
                pending_.erase(it);
            }
            p->set_value(m["result"].toObject());
        } else if (type == "select") {
            setPicking(false);
            record("element_selected", {{"selector", m["selector"]}, {"url", m["url"]}});
            QMetaObject::invokeMethod(this, [this, m] { promptForElement(m); }, Qt::QueuedConnection);
        } else if (type == "selectCancelled") {
            setPicking(false);
        } else if (type == "mutations") {
            record("dom", {{"url", m["url"]}, {"items", m["items"]}, {"dropped", m["dropped"]}});
        }
    }

    // ---- Scripting API (runs on server threads) ----
    template <class F>
    void onMain(F f) {
        if (QThread::currentThread() == thread()) f();
        else QMetaObject::invokeMethod(this, f, Qt::BlockingQueuedConnection);
    }

    // Evaluates JS (await allowed) and returns {ok, value|error}; the result travels back via the console channel.
    QJsonObject runJS(const QString& code, double timeout) {
        int id = ++nextEvalId_;
        auto promise = std::make_shared<std::promise<QJsonObject>>();
        auto future = promise->get_future();
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pending_[id] = promise;
        }
        QString wrapper = QString(R"JS(
(async () => {
  const code = %1;
  const post = (window.__curf && __curf.post) || ((m) => console.debug('__curf__:' + JSON.stringify(m)));
  const ev = (0, eval);
  const run = () => {
    try { return ev(code); } catch (e) { if (!(e instanceof SyntaxError)) throw e; }
    try { return ev('(async () => (' + code + '\n))()'); } catch (e) { if (!(e instanceof SyntaxError)) throw e; }
    return ev('(async () => {' + code + '\n})()');
  };
  let result;
  try {
    const v = await run();
    result = { ok: true, value: v === undefined ? null : JSON.parse(JSON.stringify(v) ?? 'null') };
  } catch (e) { result = { ok: false, error: String(e && e.stack || e) }; }
  post({ type: 'evalResult', id: %2, result });
})(); 0)JS").arg(jsString(code)).arg(id);
        QMetaObject::invokeMethod(this, [this, wrapper] { page_->runJavaScript(wrapper); }, Qt::QueuedConnection);
        if (future.wait_for(std::chrono::milliseconds(static_cast<long long>(timeout * 1000))) != std::future_status::ready) {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pending_.erase(id);
            return {{"ok", false}, {"error", "timeout"}};
        }
        return future.get();
    }

    QJsonObject state() {
        QJsonObject s;
        onMain([&] {
            s = {{"url", page_->url().toString()}, {"title", page_->title()}, {"loading", loading_.load()},
                 {"progress", progress_->value() / 100.0}, {"secure", isSecure()},
                 {"canGoBack", page_->history()->canGoBack()}, {"canGoForward", page_->history()->canGoForward()},
                 {"picking", pickAct_->isChecked()}};
        });
        return s;
    }

    QJsonObject waitForLoad(double timeout) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(timeout * 1000));
        QThread::msleep(250);
        while (std::chrono::steady_clock::now() < end) {
            if (!loading_) {
                QJsonObject s = state();
                s["ok"] = true;
                return s;
            }
            QThread::msleep(100);
        }
        return {{"ok", false}, {"error", "timeout waiting for page load"}};
    }

    curf::Response handle(const curf::Request& req) {
        const std::string& p = req.path;
        auto q = [&](const char* k, const char* def = "") {
            auto it = req.query.find(k);
            return it == req.query.end() ? std::string(def) : it->second;
        };
        const QString body = Q(req.body);
        const double timeout = std::stod(q("timeout", "30"));
        const bool wait = q("wait", "1") != "0";
        auto ok = [](const QJsonObject& o) { return curf::Response{200, toJSON(o)}; };
        auto js = [&](const QString& code) {
            QJsonObject r = runJS(code, timeout);
            return curf::Response{r["ok"].toBool() ? 200 : 500, toJSON(r)};
        };
        const QString selector = jsString(Q(q("selector")));

        if (p == "/" || p == "/help") {
            return ok({{"ok", true}, {"endpoints", QJsonArray{
                "GET  /state", "GET  /navigate?url=<url or search>&wait=1", "GET  /back", "GET  /forward",
                "GET  /reload", "POST /eval  (body: JavaScript, await allowed)", "GET  /html", "GET  /text",
                "GET  /title", "GET  /links", "GET  /extract?selector=<css>&attr=<name>",
                "GET  /click?selector=<css>", "POST /type?selector=<css>&submit=0  (body: text)",
                "GET  /wait?selector=<css>&timeout=30", "GET  /pick?on=1",
                "POST /annotate?selector=<css>  (body: note)", "GET  /annotations",
                "GET  /changes?since=<seq>&type=<type>", "GET  /screenshot?path=<file.png>"}}});
        }
        if (p == "/state") return ok(state());
        if (p == "/navigate") {
            QString target = req.query.count("url") ? Q(q("url")) : body;
            onMain([&] { navigate(target); });
            return ok(wait ? waitForLoad(timeout) : QJsonObject{{"ok", true}});
        }
        if (p == "/back" || p == "/forward" || p == "/reload") {
            onMain([&] {
                if (p == "/back") view_->back();
                else if (p == "/forward") view_->forward();
                else view_->reload();
            });
            return ok(wait ? waitForLoad(timeout) : QJsonObject{{"ok", true}});
        }
        if (p == "/eval") return js(body.isEmpty() ? Q(q("code")) : body);
        if (p == "/html") return js("document.documentElement.outerHTML");
        if (p == "/text") return js("document.body ? document.body.innerText : ''");
        if (p == "/title") return js("document.title");
        if (p == "/links")
            return js("Array.from(document.querySelectorAll('a[href]')).map(a => ({text: a.innerText.trim(), href: a.href}))");
        if (p == "/extract") {
            QString attr = jsString(Q(q("attr")));
            return js(QString("Array.from(document.querySelectorAll(%1)).map(e => { const d = __curf.describe(e); "
                              "if (%2) d.attr = e.getAttribute(%2); if (e.href) d.href = e.href; "
                              "if ('value' in e) d.value = e.value; return d; })").arg(selector, attr));
        }
        if (p == "/click") {
            record("script_click", {{"selector", Q(q("selector"))}});
            return js(QString("(() => { const e = document.querySelector(%1); if (!e) throw new Error('no element'); "
                              "e.scrollIntoView({block: 'center'}); e.click(); return __curf.describe(e); })()").arg(selector));
        }
        if (p == "/type") {
            record("script_type", {{"selector", Q(q("selector"))}, {"text", body}});
            QString text = jsString(body);
            return js(QString(
                "(() => { const e = document.querySelector(%1); if (!e) throw new Error('no element'); e.focus(); "
                "const proto = e instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype; "
                "const set = Object.getOwnPropertyDescriptor(proto, 'value'); "
                "if (e.isContentEditable) e.textContent = %2; else if (set) set.set.call(e, %2); else e.value = %2; "
                "e.dispatchEvent(new Event('input', {bubbles: true})); e.dispatchEvent(new Event('change', {bubbles: true})); "
                "if (%3) { if (e.form) e.form.requestSubmit ? e.form.requestSubmit() : e.form.submit(); "
                "else e.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true})); } "
                "return __curf.describe(e); })()").arg(selector, text, q("submit", "0") == "1" ? "true" : "false"));
        }
        if (p == "/wait") {
            return js(QString("new Promise((res, rej) => { const t0 = Date.now(); (function poll() { "
                              "const e = document.querySelector(%1); if (e) return res(__curf.describe(e)); "
                              "if (Date.now() - t0 > %2) return rej(new Error('timeout')); setTimeout(poll, 100); })(); })")
                          .arg(selector).arg((timeout - 0.5) * 1000));
        }
        if (p == "/pick") {
            bool on = q("on", "1") != "0";
            onMain([&] { setPicking(on); });
            return ok({{"ok", true}, {"picking", on}});
        }
        if (p == "/annotate") {
            QJsonObject r = runJS(QString("__curf.describe(document.querySelector(%1))").arg(selector), timeout);
            if (!r["value"].isObject()) return {404, toJSON({{"ok", false}, {"error", "no element matches selector"}})};
            QJsonObject saved;
            onMain([&] { saved = saveAnnotation(r["value"].toObject(), body); });
            return ok({{"ok", true}, {"annotation", saved}});
        }
        if (p == "/annotations") {
            QString file = dataDir() + "/annotations.jsonl";
            QJsonArray list;
            QFile f(file);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text))
                for (const QByteArray& line : f.readAll().split('\n'))
                    if (!line.trimmed().isEmpty()) list.append(QJsonDocument::fromJson(line).object());
            return ok({{"ok", true}, {"file", file}, {"annotations", list}});
        }
        if (p == "/changes") {
            long long since = std::stoll(q("since", "0"));
            QString type = Q(q("type"));
            QJsonArray out;
            long long last;
            {
                std::lock_guard<std::mutex> lock(changesMutex_);
                for (const QJsonObject& c : changes_)
                    if (c["seq"].toInteger() > since && (type.isEmpty() || c["type"].toString() == type)) out.append(c);
                last = seq_;
            }
            return ok({{"ok", true}, {"last", static_cast<qint64>(last)}, {"changes", out},
                       {"file", dataDir() + "/changes.jsonl"}});
        }
        if (p == "/screenshot") {
            QString path = req.query.count("path") ? Q(q("path")) : dataDir() + "/screenshot.png";
            bool saved = false;
            onMain([&] { saved = view_->grab().save(path, "PNG"); });
            return saved ? ok({{"ok", true}, {"path", path}})
                         : curf::Response{500, toJSON({{"ok", false}, {"error", "snapshot failed"}})};
        }
        return {404, toJSON({{"ok", false}, {"error", "unknown endpoint; GET / for help"}})};
    }

    void startServer() {
        server_ = std::make_unique<curf::ControlServer>(port_, [this](const curf::Request& r) { return handle(r); });
        if (server_->start()) fprintf(stderr, "curf: scripting API on http://127.0.0.1:%d\n", port_);
    }

    int port_;
    QWebEngineProfile* profile_ = nullptr;
    Page* page_ = nullptr;
    QWebEngineView* view_ = nullptr;
    QLineEdit* address_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QAction *backAct_ = nullptr, *fwdAct_ = nullptr, *reloadAct_ = nullptr, *secureAct_ = nullptr, *pickAct_ = nullptr;
    QString secureTip_, certError_;
    MixedContentInterceptor interceptor_;
    std::atomic<bool> loading_{false};

    std::mutex changesMutex_;
    std::vector<QJsonObject> changes_;
    long long seq_ = 0;

    std::mutex pendingMutex_;
    std::map<int, std::shared_ptr<std::promise<QJsonObject>>> pending_;
    std::atomic<int> nextEvalId_{0};

    std::unique_ptr<curf::ControlServer> server_;
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("curf");
    QApplication::setOrganizationName("curf");

    int port = 9333;
    QString initial;
    const QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--port" && i + 1 < args.size()) port = args[++i].toInt();
        else if (args[i] == "-h" || args[i] == "--help") {
            printf("usage: curf [--port N] [url-or-search]\n");
            return 0;
        } else if (!args[i].startsWith('-')) initial = args[i];
    }

    Browser browser(port);
    browser.show();
    if (!initial.isEmpty()) browser.navigate(initial);
    else browser.focusAddress();
    return app.exec();
}
