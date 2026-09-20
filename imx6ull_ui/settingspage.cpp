#include "settingspage.h"
#include "iostheme.h"
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QFrame>

static QFrame *makeSection(QWidget *parent, const QString &title, QVBoxLayout **bodyOut)
{
    QFrame *box = new QFrame(parent);
    box->setObjectName("panel");
    QVBoxLayout *outer = new QVBoxLayout(box);
    QLabel *head = new QLabel(title, box);
    head->setFont(Ios::fontHeadline());
    head->setStyleSheet(QString("color:%1; margin-bottom:8px;").arg(Ios::label().name()));
    outer->addWidget(head);
    QVBoxLayout *body = new QVBoxLayout;
    body->setSpacing(10);
    outer->addLayout(body);
    *bodyOut = body;
    return box;
}

QLineEdit *SettingsPage::addField(QVBoxLayout *form, const QString &label, bool password)
{
    QLabel *lab = new QLabel(label);
    lab->setFont(Ios::fontFootnote());
    lab->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
    QLineEdit *edit = new QLineEdit;
    edit->setMinimumHeight(38);
    if (password) edit->setEchoMode(QLineEdit::Password);
    form->addWidget(lab);
    form->addWidget(edit);
    return edit;
}

SettingsPage::SettingsPage(QWidget *parent) : QWidget(parent), m_mqttFromEnv(false)
{
    /* iOS 输入框：无描边、填充色底、圆角 10。
       iOS 里输入框靠"比背景亮一级的填充"来区分，不画边框——
       边框是 Material/Web 的语言。 */
    setStyleSheet(QString(
        "QLineEdit { background:%1; color:%2; border:none;"
        "  border-radius:10px; padding:11px 12px; font-size:16px; }"
        "QLineEdit:focus { background:%3; }"
        "#panel { background:%4; border:none; border-radius:16px; padding:16px; }")
        .arg(QColor(0x2C,0x2C,0x2E).name())
        .arg(Ios::label().name())
        .arg(QColor(0x3A,0x3A,0x3C).name())
        .arg(Ios::bgElevated().name()));

    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget *content = new QWidget;
    QVBoxLayout *root = new QVBoxLayout(content);
    root->setSpacing(14);

    // ---- MQTT ----
    QVBoxLayout *mqttBody;
    root->addWidget(makeSection(this, "MQTT 连接", &mqttBody));
    m_mqttHost = addField(mqttBody, "Broker 地址");
    m_mqttPort = addField(mqttBody, "端口");
    m_mqttUser = addField(mqttBody, "用户名");
    m_mqttPass = addField(mqttBody, "密码", true);
    /* 地址来自 /etc/edgemonitor.env 时的说明行。默认隐藏，setConfig 里决定显不显示。
       不显示来源、只把框置灰，用户会以为界面坏了；写清楚"改哪个文件"才是有用的提示。 */
    m_mqttEnvHint = new QLabel("由 /etc/edgemonitor.env 提供，与网关共用一份配置；"
                               "要改请改该文件（守护脚本每轮重读，无需重启）");
    m_mqttEnvHint->setWordWrap(true);
    m_mqttEnvHint->setFont(Ios::fontFootnote());
    m_mqttEnvHint->setStyleSheet(QString("color:%1;").arg(Ios::orange().name()));
    m_mqttEnvHint->hide();
    mqttBody->addWidget(m_mqttEnvHint);

    // ---- 讯飞 ----
    QVBoxLayout *xfBody;
    root->addWidget(makeSection(this, "讯飞开放平台（语音听写 / 合成 / 星火大模型）", &xfBody));
    m_xfAppId = addField(xfBody, "APPID");
    m_xfApiKey = addField(xfBody, "APIKey");
    m_xfApiSecret = addField(xfBody, "APISecret", true);
    m_xfSparkHost = addField(xfBody, "星火大模型 Host");
    m_xfSparkPath = addField(xfBody, "星火大模型 Path（对应开通的版本）");
    m_xfSparkDomain = addField(xfBody, "星火大模型 Domain（同上，需与版本一致）");

    // ---- 和风天气 ----
    QVBoxLayout *qwBody;
    root->addWidget(makeSection(this, "和风天气", &qwBody));
    m_qwHost = addField(qwBody, "API Host（登录和风天气控制台「我的项目」里查看）");
    m_qwKey = addField(qwBody, "API Key", true);
    m_qwLocation = addField(qwBody, "LocationID（默认 101010100 = 北京）");

    QPushButton *saveBtn = new QPushButton("保存并应用", content);
    saveBtn->setMinimumHeight(46);
    /* iOS 主按钮：systemBlue 实心 + 白字，圆角 14 */
    saveBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:white; border:none;"
        "  border-radius:14px; font-size:17px; font-weight:600; padding:12px; }"
        "QPushButton:pressed { background:%2; }")
        .arg(Ios::blue().name()).arg(Ios::blue().darker(120).name()));
    connect(saveBtn, &QPushButton::clicked, this, [this]() {
        AppConfig c = config();
        c.save();
        emit saved(c);
    });
    root->addWidget(saveBtn);
    root->addStretch();

    scroll->setWidget(content);
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0,0,0,0);
    outer->addWidget(scroll);
}

void SettingsPage::setConfig(const AppConfig &cfg)
{
    m_mqttHost->setText(cfg.mqttHost);
    m_mqttPort->setText(QString::number(cfg.mqttPort));
    m_mqttUser->setText(cfg.mqttUser);
    m_mqttPass->setText(cfg.mqttPass);

    /* 环境变量提供时置灰：让人在界面上改一个根本不会生效的值，比不让改更糟——
       他会以为改好了，然后去别处找"为什么还是连不上"。端口不在 env 里，保持可编辑。 */
    m_mqttFromEnv = cfg.mqttFromEnv;
    m_mqttHost->setReadOnly(m_mqttFromEnv);
    m_mqttUser->setReadOnly(m_mqttFromEnv);
    m_mqttPass->setReadOnly(m_mqttFromEnv);
    m_mqttEnvHint->setVisible(m_mqttFromEnv);
    const QString ro = m_mqttFromEnv ? "background:#141b28; color:#8a94a6;" : "";
    m_mqttHost->setStyleSheet(ro);
    m_mqttUser->setStyleSheet(ro);
    m_mqttPass->setStyleSheet(ro);

    m_xfAppId->setText(cfg.xfAppId);
    m_xfApiKey->setText(cfg.xfApiKey);
    m_xfApiSecret->setText(cfg.xfApiSecret);
    m_xfSparkHost->setText(cfg.xfSparkHost);
    m_xfSparkPath->setText(cfg.xfSparkPath);
    m_xfSparkDomain->setText(cfg.xfSparkDomain);

    m_qwHost->setText(cfg.qwHost);
    m_qwKey->setText(cfg.qwKey);
    m_qwLocation->setText(cfg.qwLocation);
}

AppConfig SettingsPage::config() const
{
    AppConfig c;
    c.mqttFromEnv = m_mqttFromEnv;   /* 原样带回，见 settingspage.h 的说明 */
    c.mqttHost = m_mqttHost->text().trimmed();
    c.mqttPort = m_mqttPort->text().toInt();
    if (c.mqttPort <= 0) c.mqttPort = 1883;
    c.mqttUser = m_mqttUser->text().trimmed();
    c.mqttPass = m_mqttPass->text();

    c.xfAppId = m_xfAppId->text().trimmed();
    c.xfApiKey = m_xfApiKey->text().trimmed();
    c.xfApiSecret = m_xfApiSecret->text().trimmed();
    c.xfSparkHost = m_xfSparkHost->text().trimmed();
    c.xfSparkPath = m_xfSparkPath->text().trimmed();
    c.xfSparkDomain = m_xfSparkDomain->text().trimmed();

    c.qwHost = m_qwHost->text().trimmed();
    c.qwKey = m_qwKey->text().trimmed();
    c.qwLocation = m_qwLocation->text().trimmed();
    return c;
}
