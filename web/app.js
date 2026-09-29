/* app.js - EdgeMonitor Control Center 前端逻辑
 *
 * v4 从单页面滚动改成侧边栏多页面控制台：仪表盘/设备控制/历史数据/告警中心/
 * OTA升级/系统设置。MQTT 协议、话题名跟之前完全一致，没有改网关端一个字节——
 * 这一版纯粹是前端信息架构和视觉的重做。
 */

// ============================================================
// 页面路由：纯前端 class 切换，不引入 SPA 框架，六个页面数据量不大，
// 没必要为了"多页面"这个需求上一整套路由库
// ============================================================
function showPage(name) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
  const page = document.getElementById('page-' + name);
  const nav = document.querySelector('.nav-item[data-page="' + name + '"]');
  if (page) page.classList.add('active');
  if (nav) nav.classList.add('active');
  const titles = {
    dashboard: ['仪表盘', '实时总览'], control: ['设备控制', '执行设备与小车'],
    history: ['历史数据', '趋势曲线与统计'], alerts: ['告警中心', '阈值与记录'],
    ota: ['OTA 升级', '固件远程升级'], settings: ['系统设置', '设备管理与连接信息'],
    screen: ['远程屏幕', '查看并操作板子的触摸屏'],
  };
  const t = titles[name] || [name, ''];
  document.getElementById('pageTitle').textContent = t[0];
  document.getElementById('pageCrumb').textContent = t[1];
  if (name === 'history') { drawChart(); drawImuChart(); loadHistory(); }
  if (name === 'alerts') loadHistoryEvents();
  if (name === 'settings') renderDeviceList();
  if (name === 'ota') loadFwList();
  // 离开远程屏幕页就停止拉流，否则会在后台一直耗流量
  if (name !== 'screen' && scrTimer) stopScreenShare();
  // 仪表盘走"整屏不滚动"的大屏布局（见 CSS 里 .page-container.no-scroll），
  // 其它页面是表单/列表，内容量不固定，还是要能滚动
  document.getElementById('pageContainer').classList.toggle('no-scroll', name === 'dashboard');
  if (name === 'dashboard') setTimeout(() => {
    if (gpsMap) gpsMap.invalidateSize();
    drawDvChart(); updateSparks();
  }, 60);
  toggleSidebar(false);   // 移动端抽屉导航：选完页面自动收起，桌面端这个 class 本来就没生效不影响
}
// 大屏面板改用弹性行高后，地图容器的实际像素高度会随窗口大小变化，
// 不像固定 280px 那样一次性算完，需要在 resize 时让 Leaflet 重新量一次
window.addEventListener('resize', () => {
  if (!document.getElementById('page-dashboard').classList.contains('active')) return;
  if (gpsMap) gpsMap.invalidateSize();
  drawDvChart();
});
document.getElementById('navList').addEventListener('click', (e) => {
  const item = e.target.closest('.nav-item');
  if (item) showPage(item.dataset.page);
});

// 移动端侧边栏抽屉：默认收起（见 CSS 里 .sidebar 在 <=860px 断点下的
// translateX(-100%)），点汉堡菜单展开成覆盖层，点遮罩/选完页面自动收起
function toggleSidebar(force) {
  const open = typeof force === 'boolean' ? force : !document.getElementById('sidebar').classList.contains('open');
  document.getElementById('sidebar').classList.toggle('open', open);
  document.getElementById('sidebarMask').classList.toggle('show', open);
}

// ============================================================
// 时钟
// ============================================================
function tickClock() {
  const now = new Date();
  document.getElementById('clock').textContent = now.toLocaleTimeString('zh-CN', { hour12:false });
  // 大屏是全屏布局，看不见后台顶栏，标题栏里自带一套时间显示
  document.getElementById('dvClock').textContent = now.toLocaleTimeString('zh-CN', { hour12:false });
  document.getElementById('dvDate').textContent =
    now.toLocaleDateString('zh-CN') + ' ' + ['周日','周一','周二','周三','周四','周五','周六'][now.getDay()];
}
setInterval(tickClock, 1000); tickClock();

// ============================================================
// Toast 通知
// ============================================================
function toast(title, msg, type) {
  type = type || 'info';
  const box = document.createElement('div');
  box.className = 'toast ' + type;
  /* 必须转义：toast 的内容里有设备名/设备 id，而它们来自 MQTT topic 和
     保留消息的 payload——任何能往 broker 发布消息的一方都能控制。
     项目里已经有 esc()，设备列表和历史事件都用了，这里漏了。
     "同一个转义函数用一半漏一半"本身就是缺陷：日后有人照着没转义的那半
     写新代码，就成了真正的注入点。 */
  box.innerHTML = '<div class="tt">' + esc(title) + '</div><div class="tm">' + esc(msg) + '</div>';
  document.getElementById('toastContainer').appendChild(box);
  setTimeout(() => {
    box.classList.add('closing');
    setTimeout(() => box.remove(), 220);
  }, 4200);
}

// 告警提示音：Web Audio API 现场合成一个简短的"滴"声，不依赖任何音频文件
let audioCtx = null;
function beep(freq, dur) {
  try {
    audioCtx = audioCtx || new (window.AudioContext || window.webkitAudioContext)();
    const osc = audioCtx.createOscillator(); const gain = audioCtx.createGain();
    osc.frequency.value = freq || 880; osc.type = 'sine';
    gain.gain.setValueAtTime(0.15, audioCtx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + (dur || 0.25));
    osc.connect(gain); gain.connect(audioCtx.destination);
    osc.start(); osc.stop(audioCtx.currentTime + (dur || 0.25));
  } catch (e) { /* 浏览器不支持或用户还没交互过页面导致 AudioContext 被拒绝，静默忽略 */ }
}

// ============================================================
// 主题切换（深色/浅色），记住选择
// ============================================================
function applyTheme(t) {
  document.body.setAttribute('data-theme', t);
  document.getElementById('themeSwitch').classList.toggle('on', t === 'light');
  try { localStorage.setItem('em_theme', t); } catch (e) {}

  /* canvas 画上去的像素不会跟着 CSS 变量走——DOM 那部分换主题是立刻生效的，
     图表却会停在旧配色里，直到下一帧数据到来才重画。
     两种颜色并存的那几秒看着就像渲染坏了，所以这里主动重绘一次。
     try 包起来：切主题时这些图表未必都已经初始化（比如还没进过历史页）。 */
  try { drawDvChart(); } catch (e) {}
  try { drawHistChart(); } catch (e) {}
}
function toggleTheme() {
  const cur = document.body.getAttribute('data-theme');
  applyTheme(cur === 'light' ? 'dark' : 'light');
}
(function initTheme() {
  let t = 'dark';
  try { t = localStorage.getItem('em_theme') || 'dark'; } catch (e) {}
  applyTheme(t);
})();

// ============================================================
// MQTT 连接
// ============================================================
const connEl = document.getElementById('conn');
const connTextEl = document.getElementById('connText');

// 连接状态要同时反映到后台顶栏和大屏标题栏两处
function setConnState(text, online) {
  connTextEl.textContent = text;
  connEl.className = 'conn-pill ' + (online ? 'online' : 'offline');
  /* 大屏标题栏空间紧张，连上之后只显示 IP——绿点已经表达了"已连接"，
     再写四个字纯属占地方，之前就是这么被挤到换行的。后台顶栏空间宽裕，
     保留完整文案 */
  const short = online ? text.replace(/^已连接\s*/, '') : text;
  document.getElementById('dvConnText').textContent = short;
  document.getElementById('dvConn').className = 'dv-chip' + (online ? '' : ' off');
}
let client = null;
let brokerHost = '';
let videoToken = '';
/* 设备在线状态。要跟"浏览器和 broker 的连接"分开记：broker 在服务器上，
   浏览器连得好好的，机器人却可能已经掉线——这恰恰是最该被看见的情况，
   旧架构里 broker 跟设备同机，这两件事分不开 */
let deviceOnline = false;
let deviceInfo = {};

/* ============================================================
 * 多机器人管理
 *
 * 设备来源有两类，缺一不可：
 *   1. 自动发现——凡是上线过的设备都会在 monitor/online/<id> 留下 retained
 *      消息，大屏一连上就能收到，不用手工登记
 *   2. 手工登记——设备离线时 broker 上可能什么都没有（比如从没上线过、
 *      或 retained 被清过），但运维仍然需要在列表里看到"这台机器人不在线"，
 *      而不是让它凭空消失
 * 所以列表 = 本地登记表 ∪ 自动发现，本地表存 localStorage。
 * ============================================================ */
let devices = {};        // id -> { id, name, online, ip, video, lastSeen, manual }
let currentDevice = '';  // 当前正在查看的设备 id

function loadDeviceRegistry() {
  try {
    const raw = localStorage.getItem('em_devices');
    if (raw) {
      JSON.parse(raw).forEach(d => {
        devices[d.id] = Object.assign({ online: false, manual: true }, d);
      });
    }
    currentDevice = localStorage.getItem('em_current_device') || '';
  } catch (e) {}
}

function saveDeviceRegistry() {
  try {
    // 只存手工登记的部分：自动发现的设备下次连上 broker 会自己回来，
    // 存下来反而会让"已经拆掉的设备"一直赖在列表里
    const list = Object.values(devices)
      .filter(d => d.manual)
      .map(d => ({ id: d.id, name: d.name, manual: true }));
    localStorage.setItem('em_devices', JSON.stringify(list));
    localStorage.setItem('em_current_device', currentDevice);
  } catch (e) {}
}

function deviceLabel(id) {
  const d = devices[id];
  return (d && d.name) ? d.name : id;
}

/* 切换当前查看的设备：退订旧设备、订阅新设备，并把界面上的实时状态清空
   —— 不清的话会短暂显示上一台设备的读数，看起来像新设备的数据 */
function switchDevice(id) {
  if (!id || id === currentDevice) return;
  if (client && client.connected && currentDevice) {
    client.unsubscribe('monitor/' + currentDevice + '/data/#');
    client.unsubscribe('monitor/' + currentDevice + '/ota/progress');
    client.unsubscribe('monitor/' + currentDevice + '/ota/status');
  }
  currentDevice = id;
  saveDeviceRegistry();

  tempData.length = 0; humiData.length = 0; distData.length = 0;
  pitchData.length = 0; rollData.length = 0; yawData.length = 0;
  frameCount = 0; lastFrameCount = 0;
  /* 传感器健康位也要清。不清的话，从一台"温湿度失联"的设备切到正常设备后，
     新设备的仪表盘会顶着上一台的失联标记，直到它自己推来一帧 imu 才纠正——
     切换瞬间显示的是别人的故障状态，比没有标记更误导 */
  sensorHealth = 0;

  const d = devices[id] || {};
  deviceOnline = !!d.online;
  deviceInfo = d;
  applyVideoSource();
  updateDeviceUi();

  if (client && client.connected) {
    client.subscribe('monitor/' + id + '/data/#');
    client.subscribe('monitor/' + id + '/ota/progress');
    client.subscribe('monitor/' + id + '/ota/status');
  }
  toast('已切换设备', deviceLabel(id), 'success');
  computeHealth();
}

function addDevice(id, name) {
  id = (id || '').trim();
  if (!id) return '设备 ID 不能为空';
  if (!/^[A-Za-z0-9_-]+$/.test(id)) return '设备 ID 只能用字母、数字、下划线和短横线（它要拼进 MQTT topic）';
  if (devices[id] && devices[id].manual) return '这个设备 ID 已经存在了';
  devices[id] = Object.assign({}, devices[id], {
    id, name: (name || '').trim() || id, manual: true,
    online: devices[id] ? devices[id].online : false
  });
  saveDeviceRegistry();
  if (!currentDevice) switchDevice(id);
  else updateDeviceUi();
  return '';
}

function removeDevice(id) {
  const d = devices[id];
  if (!d) return;
  if (d.online) {
    // 在线设备删了也会被自动发现立刻加回来，说清楚免得以为是 bug
    toast('设备仍在线', '删除的是本地登记，设备下次上报仍会自动出现', 'warn');
  }
  delete devices[id];
  saveDeviceRegistry();
  if (currentDevice === id) {
    const rest = Object.keys(devices);
    currentDevice = '';
    if (rest.length) switchDevice(rest[0]);
  }
  updateDeviceUi();
  renderDeviceList();
}

/* ============================================================
 * 语音助手密钥下发
 *
 * ── 为什么不用 retained ──
 * 原来用 retained：设备重启、或者晚于下发才上线，都能自动拿到配置。
 * 代价是**密钥会一直明文躺在 broker 上**——任何能连上 broker 的客户端
 * 订阅一下就能读走。实测确认过：订阅 monitor/# 立刻收到完整的
 * appId / apiKey / apiSecret。
 *
 * 而这个代价换来的好处其实很小：设备收到后就写进自己的 QSettings 了
 * （见 mainwindow.cpp 的 config/voice 分支），断电重启照样在。
 * retained 唯一多出来的能力，是"下发时设备恰好离线也能补上"——
 * 为这一点便利让密钥长期驻留在一个多客户端共享的 broker 上，不划算。
 *
 * 现在：非 retained 下发，设备离线时**直接拒绝并说清楚**，
 * 而不是发出去然后让人以为成功了。
 * ============================================================ */
function voiceConfigTopic() {
  return currentDevice ? 'monitor/' + currentDevice + '/config/voice' : null;
}

function pushVoiceConfig() {
  const stat = document.getElementById('xfStatus');
  if (!client || !client.connected) { stat.textContent = '未连接 broker'; return; }
  const topic = voiceConfigTopic();
  if (!topic) { stat.textContent = '未选择设备'; return; }

  const cfg = {
    appId:      document.getElementById('xfAppId').value.trim(),
    apiKey:     document.getElementById('xfApiKey').value.trim(),
    apiSecret:  document.getElementById('xfApiSecret').value.trim(),
    sparkHost:  document.getElementById('xfSparkHost').value.trim(),
    sparkPath:  document.getElementById('xfSparkPath').value.trim(),
    sparkDomain:document.getElementById('xfSparkDomain').value.trim(),
  };
  if (!cfg.appId || !cfg.apiKey || !cfg.apiSecret) {
    stat.textContent = 'APPID / APIKey / APISecret 三项都要填';
    return;
  }
  /* 设备离线就别发。非 retained 的消息没人接就是没了——
     发出去再显示"已下发"是在骗人，而这种谎最难发现：
     界面一切正常，只有到现场按语音键才发现没生效。 */
  if (!deviceOnline) {
    stat.textContent = '设备当前离线，密钥不会送达；等它上线后再点一次';
    toast('未下发', '设备离线。密钥改为非 retained 下发，离线时不会被补发', 'warn');
    return;
  }
  client.publish(topic, JSON.stringify(cfg), { retain: false, qos: 1 });
  stat.textContent = '已下发到 ' + deviceLabel(currentDevice) +
    '（' + new Date().toLocaleTimeString('zh-CN', { hour12:false }) + '）';
  toast('已下发', '设备收到后会自动保存并生效', 'success');
  // 本地只留非敏感项，密钥不写 localStorage——浏览器里存明文密钥没必要
  try {
    localStorage.setItem('em_xf_spark', JSON.stringify({
      sparkHost: cfg.sparkHost, sparkPath: cfg.sparkPath, sparkDomain: cfg.sparkDomain
    }));
  } catch (e) {}
}

function clearVoiceConfig() {
  const stat = document.getElementById('xfStatus');
  if (!client || !client.connected) { stat.textContent = '未连接 broker'; return; }
  const topic = voiceConfigTopic();
  if (!topic) { stat.textContent = '未选择设备'; return; }
  if (!confirm('清除设备上的语音密钥？\n设备将无法使用语音助手，直到重新下发。')) return;
  // 空 payload 的 retained 消息 = 删除 broker 上保留的那条
  client.publish(topic, '', { retain: true, qos: 1 });
  stat.textContent = '已清除设备上的密钥';
}

(function restoreSparkFields() {
  try {
    const s = JSON.parse(localStorage.getItem('em_xf_spark') || '{}');
    ['sparkHost', 'sparkPath', 'sparkDomain'].forEach(k => {
      const el = document.getElementById('xf' + k[0].toUpperCase() + k.slice(1));
      if (el && s[k]) el.value = s[k];
    });
  } catch (e) {}
})();

/* 设备管理列表（系统设置页） */
function renderDeviceList() {
  const box = document.getElementById('deviceList');
  if (!box) return;
  const ids = Object.keys(devices).sort();
  if (!ids.length) {
    box.innerHTML = '<div class="empty-hint">还没有任何设备。<br>' +
      '板子上的网关一旦连上 broker 就会自动出现在这里；' +
      '也可以先手工登记，方便离线时也能看到。</div>';
    return;
  }
  box.innerHTML = ids.map(id => {
    const d = devices[id];
    const cur = (id === currentDevice);
    return '<div class="dev-row' + (cur ? ' current' : '') + '">' +
      '<span class="dot ' + (d.online ? 'on' : 'off') + '"></span>' +
      '<div class="info">' +
        '<div class="nm">' + esc(d.name || id) + (cur ? ' <em>当前查看</em>' : '') + '</div>' +
        '<div class="sub">' + esc(id) +
          (d.ip ? ' · ' + esc(d.ip) : '') +
          ' · ' + (d.online ? '在线' : '离线') +
          (d.manual ? ' · 手工登记' : ' · 自动发现') +
        '</div>' +
      '</div>' +
      '<div class="ops">' +
        (cur ? '' : '<button class="ghost" onclick="switchDevice(\'' + esc(id) + '\')">切换</button>') +
        '<button class="ghost" onclick="onRenameDevice(\'' + esc(id) + '\')">改名</button>' +
        '<button class="danger" onclick="onRemoveDevice(\'' + esc(id) + '\')">删除</button>' +
      '</div>' +
    '</div>';
  }).join('');
}

function onAddDevice() {
  const idEl = document.getElementById('newDevId');
  const nameEl = document.getElementById('newDevName');
  const err = addDevice(idEl.value, nameEl.value);
  document.getElementById('devAddErr').textContent = err;
  if (!err) {
    idEl.value = ''; nameEl.value = '';
    renderDeviceList();
    toast('已添加', '设备已登记到列表', 'success');
  }
}

function onRenameDevice(id) {
  const name = prompt('新的显示名称', deviceLabel(id));
  if (name !== null) renameDevice(id, name);
}

function onRemoveDevice(id) {
  if (!confirm('确定删除设备「' + deviceLabel(id) + '」吗？\n只是从本地列表移除，不影响设备本身。')) return;
  removeDevice(id);
}

function renameDevice(id, name) {
  if (!devices[id]) return;
  devices[id].name = (name || '').trim() || id;
  devices[id].manual = true;   // 改过名字就算手工登记，删设备时才留得住
  saveDeviceRegistry();
  updateDeviceUi();
  renderDeviceList();
}
let sessionStart = null;
let frameCount = 0;
let lastFrameAt = 0;      // 最近一帧的时间戳，用来算"上报间隔"这个健康度明细
let lastFrameCount = 0;   // 上一次采样时的累计帧数，用来算 sparkline 的帧数增量

async function deriveVideoToken(user, pass) {
  const raw = user + ':' + pass;
  if (window.crypto && window.crypto.subtle && window.isSecureContext) {
    const buf = new TextEncoder().encode(raw);
    const digest = await crypto.subtle.digest('SHA-256', buf);
    const hex = Array.from(new Uint8Array(digest)).map(b => b.toString(16).padStart(2, '0')).join('');
    return hex.slice(0, 16);
  }
  // 非安全上下文（内网 http，非 localhost）下 crypto.subtle 不可用，退化成 FNV-1a，
  // 这个 token 只是防止视频地址被随手看到/转发，不是真正的安全边界，够用
  let h1 = 0x811c9dc5 >>> 0, h2 = (0x811c9dc5 ^ 0xa5a5a5a5) >>> 0;
  for (let i = 0; i < raw.length; i++) {
    const c = raw.charCodeAt(i);
    h1 = Math.imul(h1 ^ c, 16777619) >>> 0;
    h2 = Math.imul(h2 ^ (c + i), 16777619) >>> 0;
  }
  return h1.toString(16).padStart(8, '0') + h2.toString(16).padStart(8, '0');
}

function loadRemembered() {
  try {
    const host = localStorage.getItem('em_host');
    const user = localStorage.getItem('em_user');
    /* broker 地址默认用 location.hostname——这个页面就是 broker 那台机器发出来的，
       它永远是对的；记住的旧地址只有在和当前站点不同源时才有意义（把页面部署在
       别处的场景）。

       为什么不无脑用记住的值：那就是一份影子配置。虚拟机换过一次 IP 之后，
       localStorage 里的旧地址会一直遮蔽这个正确的默认值，表现是"网关和数据库
       都正常，就是网页连不上"，而且清缓存之前怎么改都没用。同样的坑在板子的
       Qt 界面上真实发生过一次（QSettings 里存着旧地址）。 */
    document.getElementById('inHost').value = host || location.hostname;
    if (user) document.getElementById('inUser').value = user;
  } catch (e) {
    try { document.getElementById('inHost').value = location.hostname; } catch (e2) {}
  }
}
loadRemembered();
loadDeviceRegistry();

async function doLogin() {
  const user = document.getElementById('inUser').value.trim();
  const pass = document.getElementById('inPass').value.trim();
  brokerHost = document.getElementById('inHost').value.trim() || location.hostname;
  const errEl = document.getElementById('loginErr');
  if (!user || !pass) { errEl.textContent = '请输入账号和密码'; return; }
  if (!brokerHost) { errEl.textContent = '请输入 broker 地址'; return; }
  /* MQTT 客户端库是从 CDN 加载的，而这套系统的部署环境经常上不了外网
     （只有内网的 VM、挂在没有出口的 AP 上的板子）。加载不到时 window.mqtt
     是 undefined，下面那句 mqtt.connect 直接抛 TypeError——登录按钮点下去
     毫无反应，控制台里一行报错，界面上什么提示都没有，用户只会以为
     "密码填错了"或者"broker 挂了"，排查方向从一开始就是错的。
     在这里就说清楚真正的原因。 */
  if (typeof mqtt === 'undefined') {
    errEl.textContent = 'MQTT 客户端库未加载：当前网络访问不到 CDN。' +
      '把 mqtt.min.js 下载到 web/ 目录并改成本地引用即可离线使用';
    return;
  }
  errEl.textContent = '连接中…';
  try { localStorage.setItem('em_host', brokerHost); localStorage.setItem('em_user', user); } catch (e) {}

  const wsUrl = (location.protocol === 'https:')
    ? 'wss://' + brokerHost + ':8084/mqtt'
    : 'ws://'  + brokerHost + ':8083/mqtt';
  client = mqtt.connect(wsUrl, { username: user, password: pass, reconnectPeriod: 3000 });

  /* 连不上时，如果用的是记住的旧地址，自动回退到当前站点的主机名试一次。
     只回退一次（brokerFellBack 挡住循环），并且明确告诉用户发生了什么——
     静默改地址会让人以为自己填的那个是通的，下次换环境又踩同一个坑。 */
  if (brokerHost !== location.hostname && location.hostname) {
    let brokerFellBack = false;
    const fallbackTimer = setTimeout(() => {
      if (brokerFellBack || (client && client.connected)) return;
      brokerFellBack = true;
      const old = brokerHost;
      try { client.end(true); } catch (e) {}
      try { localStorage.setItem('em_host', location.hostname); } catch (e) {}
      errEl.textContent = '连不上 ' + old + '，已自动改用当前站点地址 '
                        + location.hostname + '，请重新登录';
      document.getElementById('inHost').value = location.hostname;
    }, 6000);
    client.on('connect', () => clearTimeout(fallbackTimer));
  }

  client.on('message', (topic, payload) => {
    let data;
    try { data = JSON.parse(payload.toString()); } catch (e) { return; }

    /* 在线通告要在计帧之前处理：它是 retained 消息，一连上就会立刻收到
       一条历史消息，不该被算成"刚收到一帧实时数据" */
    if (topic.startsWith('monitor/online/')) {
      onDeviceOnline(topic.split('/')[2], data);
      return;
    }

    /* OTA 的两个 topic 必须在这里就分流掉，不能留到下面。
       下面用的是 topic.endsWith('/status') 这种后缀匹配，而
       ".../ota/status" 同样以 /status 结尾——会被当成传感器状态帧
       喂给 onStatusData()，于是升级失败的原因在网页上凭空消失，
       只能翻板子日志才看得到。后缀匹配对 topic 路由来说太宽松了。

       topic 现在带设备号（monitor/<dev>/ota/xxx）：升级会让 STM32 复位并
       擦写 Flash，是最不能"顺带波及"到别的设备的操作，命令必须点对点。
       进度也得分开，否则两台车同时升级时进度条互相覆盖。 */
    if (topic.indexOf('/ota/') > 0) {
      if (topic.endsWith('/ota/progress')) { updateOtaProgress(data); return; }
      if (topic.endsWith('/ota/status'))   { finishOta(data); return; }
    }

    /* 数据 topic 形如 monitor/<dev>/data/xxx。虽然只订阅了当前设备，
       但 broker 重连、切换设备的间隙可能收到别的设备的残留消息，
       这里再挡一道，避免把别人的数据画到当前设备的曲线上 */
    const parts = topic.split('/');
    if (parts.length >= 4 && parts[0] === 'monitor' && parts[2] === 'data') {
      if (parts[1] !== currentDevice) return;
    }

    frameCount++;
    document.getElementById('statFrames').textContent = frameCount;
    document.getElementById('kpiFrames').textContent = frameCount;

    if (topic.endsWith('/status')) {
      onStatusData(data);
    } else if (topic.endsWith('/imu')) {
      /* hf 走 IMU 帧只是因为那一帧还有空位，它描述的是全部四路传感器 */
      applySensorHealth(data.hf);
      applyBattery(data.bat);
      const p = num(data.p), r = num(data.r), y = num(data.y);
      updateHorizon(p, r, y);
      pushImuChart(p, r, y);
    } else if (topic.endsWith('/motor')) {
      /* 同 onStatusData：先收敛成数字。缺字段时原来会往界面上直接写
         "undefined"，条形图那边则算出 NaN%，宽度变成 0——看着像"停着不动"，
         而不是"数据有问题"，属于最糟的那种失败方式 */
      const rl = Math.round(num(data.rl)), rr = Math.round(num(data.rr));
      const ol = Math.round(num(data.ol)), orr = Math.round(num(data.or));
      setText('vRpmL', rl); setText('vRpmR', rr); setText('vOdoL', ol); setText('vOdoR', orr);
      // 条形长度：转速按 200RPM 满格；里程没有天然上限，用当前两轮的最大值
      // 做相对比例，重点是看左右轮跑得齐不齐，而不是绝对值
      const odoMax = Math.max(ol, orr, 1);
      setBar('barRpmL', (rl / 200) * 100);
      setBar('barRpmR', (rr / 200) * 100);
      setBar('barOdoL', (ol / odoMax) * 100);
      setBar('barOdoR', (orr / odoMax) * 100);
      const moving = rl > 0 || rr > 0;
      document.getElementById('motorHint').textContent = moving ? '运行中' : '待机';
      document.getElementById('statOdoL').innerHTML = ol + '<span class="unit">cm</span>';
      document.getElementById('statOdoR').innerHTML = orr + '<span class="unit">cm</span>';
    } else if (topic.endsWith('/gps')) {
      updateGps(data);
    }
  });

  client.on('connect', async () => {
    document.getElementById('loginOverlay').style.display = 'none';
    document.getElementById('appShell').classList.add('show');
    setConnState('已连接 ' + brokerHost, true);
    document.getElementById('cfgHost').textContent = brokerHost;
    document.getElementById('cfgUser').textContent = user;
    document.getElementById('userName').textContent = user;
    document.getElementById('userHost').textContent = brokerHost;
    document.getElementById('userAvatar').textContent = user.charAt(0).toUpperCase();
    sessionStart = Date.now();
    /* 先订阅在线通告（retained，能立刻发现所有设备），当前设备的数据
       在 switchDevice 里按需订阅——不订阅全部设备的数据是有意的：
       几十台设备的实时流全推给浏览器没有意义，只看当前这台就够 */
    client.subscribe('monitor/online/#');
    /* OTA 的进度/结果跟着当前设备走，见 switchDevice() 里的订阅 */
    if (currentDevice) {
      client.subscribe('monitor/' + currentDevice + '/data/#');
      client.subscribe('monitor/' + currentDevice + '/ota/progress');
      client.subscribe('monitor/' + currentDevice + '/ota/status');
    }
    /* 视频地址不在这里拼了：broker 现在部署在服务器侧，而视频服务仍在
       机器人本机上，两者不再是同一个地址。改由设备上线时自报 IP，
       收到 monitor/online/# 后再挂视频源，见 onDeviceOnline() */
    videoToken = await deriveVideoToken(user, pass);
    log('已连接 MQTT broker，等待设备上线通告');
    toast('已连接', 'MQTT broker ' + brokerHost, 'success');
    if (!gpsMap) initMap();
  });
  client.on('close', () => {
    setConnState('已断开', false);
  });
  client.on('error', () => { errEl.textContent = '连接失败：请检查账号密码或 broker 状态'; });
}

/* 设备上线/掉线通告。上线是网关自己发的，掉线是 broker 按遗嘱代发的——
   后者才是关键：拔网线/断电/进程被杀时，设备自己已经发不出任何消息了 */
function onDeviceOnline(id, d) {
  if (!id) return;
  const online = (d.online === 1);
  const prev = devices[id];
  const wasOnline = prev ? prev.online : false;

  // 自动发现：没登记过的设备直接进列表（manual=false，不写 localStorage）
  devices[id] = Object.assign({}, prev, {
    id,
    name: (prev && prev.name) ? prev.name : id,
    manual: prev ? !!prev.manual : false,
    online,
    ip: d.ip || (prev && prev.ip),
    video: d.video || (prev && prev.video) || 8081,
    lastSeen: Date.now()
  });

  // 还没选设备时，第一个上线的自动成为当前设备
  if (!currentDevice) { switchDevice(id); renderDeviceList(); return; }

  if (id === currentDevice) {
    deviceOnline = online;
    deviceInfo = devices[id];
    applyVideoSource();
    if (wasOnline && !online) {
      raiseAlert('devoff', 'danger', '设备离线', deviceLabel(id) + ' 已断开连接（broker 遗嘱触发）');
    } else if (!wasOnline && online) {
      toast('设备上线', deviceLabel(id) + ' @ ' + (d.ip || '未知地址'), 'success');
    }
    computeHealth();
  } else if (!wasOnline && online) {
    // 非当前设备也要提示，否则新机器人上线了完全没人知道
    toast('其它设备上线', deviceLabel(id) + '（当前正在查看 ' + deviceLabel(currentDevice) + '）', 'info');
  }

  updateDeviceUi();
  renderDeviceList();
}

/* 视频源按当前设备的通告地址挂载。broker 在服务器、视频服务在各自的机器人
   本机，所以地址只能来自设备自报，不能用 broker 地址拼 */
/* 视频断线自动重连。
 *
 * MJPEG 是用一个 <img> 拉的一条**不结束的** HTTP 响应。它有个很坑的特性：
 * 连接一旦断过一次（板子换 IP 的那几秒、WiFi 抖一下、视频服务重启），
 * <img> 就停在碎图上，**永远不会自己重试**——只能刷新整个页面。
 * 大屏是挂在墙上长期看的，没人会去刷新，于是一次几秒钟的抖动
 * 就变成了"视频一直是坏的"。
 *
 * 所以在 onerror 里接管：先把碎图藏起来、换成"正在重连"的说明，
 * 再退避重试（3s 起步，翻倍，封顶 15s）。
 * 重试时 URL 带一个时间戳：同一个 src 字符串重新赋值，浏览器可能直接复用
 * 失败的结果而不发新请求。 */
let videoRetryTimer = null;
let videoRetryDelay = 3000;

function setVideoEmptyText(title, sub) {
  const empty = document.getElementById('videoEmpty');
  if (!empty) return;
  const b = empty.querySelector('b'), sp = empty.querySelector('span');
  if (b && empty.dataset.t0 === undefined) {
    /* 第一次改之前把原文存下来，恢复时用——不在 JS 里再抄一份文案，
       否则 HTML 里改了措辞，这里的备份就悄悄过时了 */
    empty.dataset.t0 = b.textContent;
    empty.dataset.s0 = sp ? sp.textContent : '';
  }
  if (b) b.textContent = title !== undefined ? title : empty.dataset.t0;
  if (sp) sp.textContent = sub !== undefined ? sub : empty.dataset.s0;
}

function applyVideoSource() {
  const stream = document.getElementById('stream');
  const empty = document.getElementById('videoEmpty');
  if (!stream) return;
  const d = devices[currentDevice];
  const ok = !!(d && d.online && d.ip);
  clearTimeout(videoRetryTimer);
  if (ok) {
    stream.onerror = function () {
      const d2 = devices[currentDevice];
      if (!(d2 && d2.online && d2.ip)) return;   /* 设备真离线了就别重试，交给离线态 */
      stream.hidden = true;
      if (empty) empty.classList.remove('hide');
      setVideoEmptyText('视频连接中断',
        Math.round(videoRetryDelay / 1000) + ' 秒后自动重连（经服务器中转 → ' + d2.ip + ':8081）');
      videoRetryTimer = setTimeout(function () {
        videoRetryDelay = Math.min(videoRetryDelay * 2, 15000);
        applyVideoSource();
      }, videoRetryDelay);
    };
    /* 拿到第一帧就算恢复：退避时间归位，说明文字还原 */
    stream.onload = function () {
      videoRetryDelay = 3000;
      setVideoEmptyText();
    };
    /* 走服务器的 /video 中转，**不直接连板子**。
       直连要求看大屏的设备能访问到板子的局域网 IP：浏览器会限制网页去访问
       局域网里的另一台设备（实测从 fly260305.local 打开页面时被直接拦掉），
       通过 Tailscale 在外面访问时更是根本到不了 192.168.100.x。
       同源之后浏览器只跟一个地址打交道，这些问题一起消失。
       见 server/web_server.py。 */
    stream.src = '/video?host=' + encodeURIComponent(d.ip) +
                 '&token=' + encodeURIComponent(videoToken) +
                 '&_=' + Date.now();
  } else {
    setVideoEmptyText();
    stream.removeAttribute('src');   // 设备离线还留着画面会让人以为是实时的
  }
  /* 用 hidden 而不是把 src 清空了事：没有 src 的 <img> 会被浏览器画成碎图，
     那是"页面坏了"的信号，而这里其实一切正常，只是设备不在线 */
  stream.hidden = !ok;
  if (empty) empty.classList.toggle('hide', ok);
}

/* 顶栏的设备状态芯片 + 设备下拉框 */
function updateDeviceUi() {
  /* devState 现在是"状态点 + 下拉"的容器，只切在线/离线的样式，
     设备名由下拉自己显示，不再重复写一遍文字（顶栏空间有限） */
  const el = document.getElementById('devState');
  if (el) el.className = 'dv-devbox' + (deviceOnline ? '' : ' off');

  const sel = document.getElementById('devSelect');
  if (sel) {
    const ids = Object.keys(devices).sort();
    sel.innerHTML = ids.length
      ? ids.map(id => '<option value="' + esc(id) + '"' + (id === currentDevice ? ' selected' : '') + '>' +
          esc(deviceLabel(id)) + (devices[id].online ? '' : '（离线）') + '</option>').join('')
      : '<option value="">暂无设备</option>';
  }
}

function doLogout() {
  if (client) { client.end(true); client = null; }
  document.getElementById('loginOverlay').style.display = 'flex';
  document.getElementById('appShell').classList.remove('show');
  document.getElementById('inPass').value = '';
  document.getElementById('stream').src = '';
  setConnState('未连接', false);
  log('已登出');
}

// ============================================================
// 传感器数据接收 + 健康度评分 + 告警引擎
// ============================================================
const ledState = { on:false };
const buzzerState = { on:false };
let thresholds = { tempHigh:40, distLow:10, humiHigh:90 };
(function loadThresholds() {
  try {
    const s = localStorage.getItem('em_thresholds');
    if (s) thresholds = Object.assign(thresholds, JSON.parse(s));
  } catch (e) {}
  document.getElementById('thTempHigh').value = thresholds.tempHigh;
  document.getElementById('thDistLow').value = thresholds.distLow;
  document.getElementById('thHumiHigh').value = thresholds.humiHigh;
})();
function saveThresholds() {
  thresholds.tempHigh = parseFloat(document.getElementById('thTempHigh').value) || 40;
  thresholds.distLow = parseFloat(document.getElementById('thDistLow').value) || 10;
  thresholds.humiHigh = parseFloat(document.getElementById('thHumiHigh').value) || 90;
  try { localStorage.setItem('em_thresholds', JSON.stringify(thresholds)); } catch (e) {}
  toast('已保存', '告警阈值已更新', 'success');
  // 就地给一句反馈，不用只靠角落里一闪而过的 toast
  const el = document.getElementById('thSaved');
  if (el) {
    el.textContent = '已保存于 ' + new Date().toLocaleTimeString('zh-CN', { hour12:false });
    setTimeout(() => { el.textContent = ''; }, 4000);
  }
}

const alerts = [];               // {level, title, msg, ts}
const alertCooldown = {};        // key -> 上次触发时间戳，避免同一条件反复刷屏
const COOLDOWN_MS = 60000;

function raiseAlert(key, level, title, msg) {
  const now = Date.now();
  if (alertCooldown[key] && now - alertCooldown[key] < COOLDOWN_MS) return;
  alertCooldown[key] = now;
  alerts.unshift({ level, title, msg, ts: now });
  if (alerts.length > 100) alerts.pop();
  renderAlerts();
  toast(title, msg, level);
  beep(level === 'danger' ? 660 : 520, level === 'danger' ? 0.35 : 0.2);
}
function clearAlerts() {
  alerts.length = 0;
  renderAlerts();
}
/* 告警中心顶部的概览。要区分"当前生效"和"累计"——运维关心的是
   此刻有没有事，而不是这一趟总共报过多少条 */
function updateAlertKpis() {
  const now = Date.now();
  const active = alerts.filter(a => now - a.ts < 5 * 60000).length;
  const danger = alerts.filter(a => a.level === 'danger').length;
  const warn = alerts.filter(a => a.level === 'warn').length;
  const set = (id, v) => { const e = document.getElementById(id); if (e) e.querySelector('b').textContent = v; };
  set('akpiActive', active);
  set('akpiDanger', danger);
  set('akpiWarn', warn);
  const lastEl = document.getElementById('akpiLast');
  if (lastEl) {
    const a = alerts[0];
    lastEl.querySelector('b').textContent = a
      ? new Date(a.ts).toLocaleTimeString('zh-CN', { hour12:false }).slice(0, 5) : '--';
    const msgEl = document.getElementById('akpiLastMsg');
    if (msgEl) msgEl.textContent = a ? a.title : '暂无';
  }
}

/* 阈值旁边显示当前实测值，填的时候有个参照 */
function updateThresholdRefs(d) {
  const set = (id, v) => { const e = document.getElementById(id); if (e && v !== undefined) e.textContent = v; };
  set('thNowTemp', d.t !== undefined ? d.t.toFixed(1) + ' ℃' : undefined);
  set('thNowDist', d.d !== undefined ? d.d + ' cm' : undefined);
  set('thNowHumi', d.h !== undefined ? d.h.toFixed(1) + ' %' : undefined);
}

function renderAlerts() {
  updateAlertKpis();
  const unread = alerts.length;
  const badge = document.getElementById('navAlertBadge');
  const bell = document.getElementById('bellBtn');
  if (unread > 0) { badge.style.display='inline-block'; badge.textContent = unread > 99 ? '99+' : unread; bell.classList.add('has-alert'); }
  else { badge.style.display='none'; bell.classList.remove('has-alert'); }

  const list = document.getElementById('alertList');
  if (alerts.length === 0) { list.innerHTML = '<div class="empty-hint">暂无告警，一切正常</div>'; }
  else {
    list.innerHTML = alerts.map(a => (
      '<div class="alert-item ' + a.level + '">' +
        '<div class="ic"></div>' +
        /* 同 toast：a.msg 里可能带设备名（见 raiseAlert('devoff', ...)） */
        '<div class="body"><div class="t">' + esc(a.title) + '</div><div class="m">' + esc(a.msg) + '</div></div>' +
        '<div class="ts">' + new Date(a.ts).toLocaleTimeString() + '</div>' +
      '</div>'
    )).join('');
  }

  document.getElementById('kpiAlerts').textContent = alerts.length;

  // 大屏右列的实时告警，跟告警中心页共用同一份 alerts 数据
  const mini = document.getElementById('dvAlertMini');
  if (mini) {
    if (alerts.length === 0) {
      /* 空状态是写死在 HTML 里的（带图标那一版），这里只负责显示/隐藏，
         不再用 innerHTML 现拼一行小字——否则同一个"没有数据"的状态在
         项目里会有两套不同的样子 */
      /* 顺序要紧：必须先取到模板节点再清空容器。反过来的话，
         innerHTML='' 已经把 HTML 里那份 #alertEmpty 从 DOM 里删掉了，
         alertEmptyNode() 第一次调用就只能拿到 null，缓存下一个空 div，
         之后永远显示不出空状态 */
      const emptyNode = alertEmptyNode();
      mini.innerHTML = '';
      mini.appendChild(emptyNode);
    } else {
      mini.innerHTML = alerts.slice(0, 8).map(a => (
        '<div class="dv-alert-mini ' + a.level + '">' +
          '<span class="dot"></span>' +
          '<div class="body">' +
            '<div class="t">' + esc(a.title) +
              '<em>' + new Date(a.ts).toLocaleTimeString('zh-CN', { hour12:false }) + '</em></div>' +
            '<div class="m">' + esc(a.msg) + '</div>' +
          '</div>' +
        '</div>'
      )).join('');
    }
  }
}

/* 大屏告警面板的空状态节点。第一次调用时把 HTML 里那份原样存下来，
   之后重复使用——比在 JS 里再拼一遍 SVG 可靠：样式只有一个来源 */
let _alertEmptyTpl = null;
function alertEmptyNode() {
  if (!_alertEmptyTpl) {
    const n = document.getElementById('alertEmpty');
    _alertEmptyTpl = n ? n.cloneNode(true) : document.createElement('div');
  }
  return _alertEmptyTpl.cloneNode(true);
}

function computeHealth() {
  let score = 100;
  const now = Date.now();
  const activeAlerts = alerts.filter(a => now - a.ts < 5 * 60000); // 最近5分钟内的算"当前生效"
  activeAlerts.forEach(a => { score -= (a.level === 'danger' ? 22 : 10); });
  if (!client || !client.connected) score -= 30;   // 大屏连不上 broker
  if (!deviceOnline) score -= 40;                  // 设备本身离线，比前者更严重：一点数据都没有
  score = Math.max(0, Math.min(100, score));

  const ring = document.getElementById('healthRing');
  ring.style.setProperty('--pct', score);
  document.getElementById('healthScore').textContent = score;
  document.getElementById('kpiHealth').textContent = score;
  const titleEl = document.getElementById('healthTitle');
  const detailEl = document.getElementById('healthDetail');
  if (score >= 90) titleEl.textContent = '系统状态良好';
  else if (score >= 70) titleEl.textContent = '系统状态基本正常';
  else if (score >= 40) titleEl.textContent = '存在异常，请关注';
  else titleEl.textContent = '状态较差，建议立即检查';
  detailEl.textContent = activeAlerts.length
    ? ('最近 5 分钟内 ' + activeAlerts.length + ' 条告警')
    : '最近 5 分钟内无告警';
  document.getElementById('healthHint').textContent = new Date().toLocaleTimeString();

  // 评分明细：把扣分项摊开，不然只有一个分数，看不出为什么是这个分
  const online = !!(client && client.connected);
  const link = document.getElementById('hsLink');
  /* 分开显示，才看得出问题出在哪一段：是大屏到 broker 断了，
     还是 broker 到机器人这一段断了 */
  link.textContent = !online ? 'broker 断开' : (deviceOnline ? '正常' : '设备离线');
  link.className = (online && deviceOnline) ? '' : 'bad';
  const al = document.getElementById('hsAlerts');
  al.textContent = activeAlerts.length + ' 条';
  al.className = activeAlerts.length === 0 ? '' : (activeAlerts.some(a => a.level === 'danger') ? 'bad' : 'warn');
  document.getElementById('hsRate').textContent = lastFrameAt
    ? (Math.round((Date.now() - lastFrameAt) / 100) / 10) + ' s'
    : '--';
  return score;
}

// 大屏仪表环：同时更新数值文字、圆环填充百分比、超阈值变色。四个指标的
// "满量程"是按常见场景拍的经验值，不是什么协议规定的范围，纯粹为了让
// 圆环视觉上有合理的填充比例——温度/光照/湿度是"数值越大环越满"，
// 障碍距离反过来做成"越靠近环越满"（更符合仪表盘"进红区=危险"的通用
// 直觉，不是字面数值大小的直译，正常安全距离下环应该是接近空的）
/* 传感器健康位，由 STM32 经 CAN(0x101 的 data[6]) -> 网关 -> MQTT 的
   data/imu 里的 hf 字段送上来。置 1 = 该路连续读取失败，显示的是陈旧值。
   见固件 app_task.h 的 SENS_FAULT_* */
const SENS_FAULT = { th: 0x01, dist: 0x02, light: 0x04, imu: 0x08, motor: 0x10 };
let sensorHealth = 0;
function isStale(bit) { return (sensorHealth & bit) !== 0; }

function setGauge(ringId, valId, text, unit, pct, level, staleBit) {
  const val = document.getElementById(valId);
  const stale = staleBit !== undefined && isStale(staleBit);
  if (val) {
    /* 陈旧时在数值后面挂一个明确的标记。只把颜色调暗是不够的——
       用户看到一个数字，默认就会相信它是当前值，必须用文字点破 */
    val.innerHTML = text + '<span class="u">' + unit + '</span>'
                  + (stale ? '<span class="stale-tag">已失联</span>' : '');
  }
  const ring = document.getElementById(ringId);
  if (!ring) return;
  ring.style.setProperty('--pct', Math.max(0, Math.min(100, pct)));
  ring.classList.remove('warn', 'danger', 'stale');
  if (stale) ring.classList.add('stale');
  else if (level) ring.classList.add(level);
}

/* 收到新的健康位时调用。只在【发生变化】时告警，否则传感器坏着的每一帧
   都会刷一条，告警列表几秒就被淹没 */
function applySensorHealth(hf) {
  if (typeof hf !== 'number' || hf === sensorHealth) { sensorHealth = hf | 0; return; }
  const prev = sensorHealth;
  sensorHealth = hf;
  const names = [
    [SENS_FAULT.th, '温湿度传感器'],
    [SENS_FAULT.dist, '超声波测距'],
    [SENS_FAULT.light, '光照传感器'],
    [SENS_FAULT.imu, '姿态传感器'],
    /* bit4 不是"读不到数"，而是电机驱动初始化失败或**飞车保护已触发**。
       后者必须看得见：不报的话大屏上只有"转速 0"，和正常停车一模一样。 */
    [SENS_FAULT.motor, '电机驱动'],
  ];
  names.forEach(([bit, name]) => {
    const was = (prev & bit) !== 0, now = (hf & bit) !== 0;
    if (!was && now) raiseAlert('sens' + bit, 'danger',
      name + (bit === SENS_FAULT.motor ? '异常' : '失联'),
      bit === SENS_FAULT.motor
        ? '电机驱动初始化失败，或飞车保护已触发（STBY 已拉低，电机硬停）。'
          + '需要人工确认后下发解除命令才会恢复'
        : name + '连续读取失败，界面上该项显示的是最后一次有效读数，不是当前值');
    else if (was && !now) raiseAlert('sens' + bit, 'info', name + '已恢复',
      name + '重新开始正常上报');
  });
}

/* 电池电压。单位 V，**0 = ADC 线未接**（不是 0V）。
 *
 * 为什么要把"未接线"单独拎出来：把它显示成 0.0V 就是一条永不消失的低
 * 电量告警。**假告警看多了，真告警也就没人信了**——这比没有告警更糟。
 *
 * 阈值按 3S 锂电池（标称 11.1V，满电 12.6V）：
 *   < 10.5V 危险（接近截止，再放会损伤电芯）
 *   < 11.1V 提醒
 * 换铅酸或 4S 要改这两个数。告警只在**等级变化时**发一次，
 * 同 applySensorHealth：每帧都发的话告警列表几秒就被淡没了。 */
var battLevel = null;
function applyBattery(v) {
  const el = document.getElementById('hsBatt');
  if (!el) return;
  const bat = num(v, -1);

  if (bat <= 0) {                      /* 未接线 */
    el.textContent = '未接入';
    el.style.color = '';
    battLevel = null;                  /* 不告警，也不算恢复 */
    return;
  }

  el.textContent = bat.toFixed(1) + ' V';
  let lv = 'ok';
  if (bat < 10.5)      lv = 'danger';
  else if (bat < 11.1) lv = 'warn';
  el.style.color = (lv === 'danger') ? 'var(--c-danger)'
                 : (lv === 'warn')   ? 'var(--c-warn)' : '';

  if (lv !== battLevel) {
    if (lv === 'danger') raiseAlert('batt', 'danger', '电池电压过低',
      '当前 ' + bat.toFixed(1) + 'V，已接近截止电压，继续放电会损伤电芯，请立即回充');
    else if (lv === 'warn') raiseAlert('batt', 'warn', '电池电量偏低',
      '当前 ' + bat.toFixed(1) + 'V，建议安排回充');
    else if (battLevel) raiseAlert('batt', 'info', '电池电压恢复',
      '当前 ' + bat.toFixed(1) + 'V');
    battLevel = lv;
  }
}

/* 把上报值收敛成一个安全的数字。
   看起来多此一举，但这一层是必须的：原来直接写 data.t.toFixed(1)，只要某一帧
   里 t 缺失或是 null，就抛 TypeError——而这个函数后面还要更新 LED/蜂鸣器状态、
   推曲线、判告警，异常一抛整段就断在中间。表现是"仪表盘突然不动了"，
   控制台里一行报错，页面上毫无提示，很容易被当成"设备离线"。
   上游数据不可信，展示层就该自己兜住。 */
function num(v, fallback) {
  const n = Number(v);
  return Number.isFinite(n) ? n : (fallback === undefined ? 0 : fallback);
}

function onStatusData(data) {
  lastFrameAt = Date.now();
  const temp = num(data.t);
  const humi = num(data.h);
  /* 光照和距离是整数量纲，四舍五入再显示：网关目前发的就是整数，但万一哪天
     改成了平均值，界面上会直接冒出 66.72537654233024 这种一长串小数 */
  const light = Math.round(num(data.l));
  const dist = Math.round(num(data.d));
  updateThresholdRefs({ t: temp, d: dist, h: humi });
  setGauge('gTemp', 'vTemp', temp.toFixed(1), '℃', (temp / 50) * 100, temp > thresholds.tempHigh ? 'danger' : null, SENS_FAULT.th);
  setGauge('gHumi', 'vHumi', humi.toFixed(1), '%', humi, humi > thresholds.humiHigh ? 'warn' : null, SENS_FAULT.th);
  setGauge('gLight', 'vLight', light, '%', light, null, SENS_FAULT.light);
  setGauge('gDist', 'vDist', dist, 'cm', 100 - (dist / 200) * 100, dist < thresholds.distLow ? 'warn' : null, SENS_FAULT.dist);

  const s = num(data.s);
  checkCmdAcks(s);   /* 有命令在等回执的话，在这里兑现 */
  ledState.on = (s & 1) === 1; buzzerState.on = (s & 2) === 2;
  setDot('dotLed', ledState.on); setDot('dotBuzzer', buzzerState.on);
  setDot('dotMotor', (s & 4) === 4);
  document.getElementById('ledBtn').textContent = (ledState.on ? '💡 LED：开' : '💡 LED：关');
  document.getElementById('buzzerBtn').textContent = (buzzerState.on ? '🔔 蜂鸣器：开' : '🔔 蜂鸣器：关');
  pushChart(temp, humi, dist);

  /* 阈值告警必须跳过已失联的传感器。陈旧值是"最后一次有效读数"，
     拿它去判阈值毫无意义：传感器在 41℃ 时坏掉，之后会永远重复报
     "温度过高"，而真实温度早就降下来了——一条永不消失的假告警比没有
     告警更糟，它会让人对整个告警系统失去信任 */
  if (!isStale(SENS_FAULT.th) && temp > thresholds.tempHigh)
    raiseAlert('temp', 'danger', '温度过高', '当前 ' + temp.toFixed(1) + '℃，超过阈值 ' + thresholds.tempHigh + '℃');
  if (!isStale(SENS_FAULT.dist) && dist < thresholds.distLow)
    raiseAlert('dist', 'warn', '碰撞预警', '前方距离仅 ' + dist + 'cm，低于阈值 ' + thresholds.distLow + 'cm');
  if (!isStale(SENS_FAULT.th) && humi > thresholds.humiHigh)
    raiseAlert('humi', 'warn', '湿度异常', '当前湿度 ' + humi.toFixed(1) + '%，超过阈值 ' + thresholds.humiHigh + '%');

  const score = computeHealth();
  let sec = 0;
  if (sessionStart) {
    sec = Math.floor((Date.now() - sessionStart) / 1000);
    const mm = String(Math.floor(sec / 60)).padStart(2, '0'), ss = String(sec % 60).padStart(2, '0');
    document.getElementById('statUptime').textContent = mm + ':' + ss;
    document.getElementById('kpiUptime').textContent = mm + ':' + ss;
  }

  // sparkline 历史。帧数记的是"这次上报间隔内新增了多少帧"，
  // 累计值画成柱状只会单调递增，看不出波动
  pushSpark('Uptime', sec);
  pushSpark('Frames', frameCount - lastFrameCount);
  pushSpark('Alerts', alerts.length);
  pushSpark('Health', score);
  lastFrameCount = frameCount;
  if (document.getElementById('page-dashboard').classList.contains('active')) updateSparks();
}
function setText(id, v) {
  if (v === undefined) return;
  const el = document.getElementById(id);
  if (el) el.textContent = v;
}
function setBar(id, pct) {
  const el = document.getElementById(id);
  if (el) el.style.width = Math.max(0, Math.min(100, pct)) + '%';
}
function setDot(id, on) { const e = document.getElementById(id); e.className = 'status-dot ' + (on ? 'on' : 'off'); }

// ============================================================
// 姿态水平仪 + 罗盘
// ============================================================
function updateHorizon(pitch, roll, yaw) {
  const sky = document.getElementById('horizonSky');
  // pitch 上下平移模拟俯仰（限幅避免转出画面），roll 用旋转
  const clampedPitch = Math.max(-45, Math.min(45, pitch));
  sky.style.transform = 'translateY(' + (clampedPitch * 1.6) + 'px) rotate(' + (-roll) + 'deg)';
  /* 航向归一化到 0~360。IMU 给的是累积角，会出现 -141° 或 725° 这种值——
     数学上等价，但人读罗盘时要换算一下才知道朝哪。 */
  const hdg = ((yaw % 360) + 360) % 360;
  /* translateY 必须和 rotate 写在一起：内联 transform 会整个覆盖 CSS 里的 transform。
     指针以底边为轴转，而底边默认在罗盘中心下方半个针长处——
     先上移半个针长，轴心才落在罗盘正中。 */
  document.getElementById('compassNeedle').style.transform =
    'translateY(-11px) rotate(' + hdg + 'deg)';
  /* **标明是"相对航向"**。MPU6050 没有磁力计，偏航角是陀螺从开机那一刻
     积分出来的：0° 是"上电时车头的朝向"，不是北，而且会随时间漂移。
     罗盘的外观天然暗示"指北"，不写清楚就会被当成真航向用。 */
  document.getElementById('horizonLabel').textContent =
    '俯仰 ' + pitch.toFixed(1) + '°  横滚 ' + roll.toFixed(1) + '°  航向 ' + hdg.toFixed(0) + '°（相对上电朝向）';
}

// ============================================================
// 控制下发
// ============================================================
/* ---------- 命令闭环确认 ----------
 *
 * 点一下"LED 开"，界面立刻就变成"开"——但那只是【乐观更新】，它证明的
 * 仅仅是"消息发出去了"。真实链路是：
 *     浏览器 -> MQTT broker -> 网关 -> CAN -> STM32 -> 状态帧 -> 一路回来
 * 中间任何一环断了（网关挂了、CAN 没接、STM32 复位中），界面照样显示"已开"，
 * 而灯根本没亮。对一个远程控制系统来说，这是最不能接受的一类错觉：
 * 操作者以为自己控制住了设备，其实没有。
 *
 * 所以这里等设备把状态位回传回来才算数，并顺便量出整条链路的往返时延——
 * 那个数字本身也是有用的诊断信息：平时一百多毫秒，突然涨到两秒就说明
 * 链路上有东西堵了。
 */
const cmdLog = [];            // 最近的命令及其确认状态，渲染在控制页
const pendingCmds = [];       // 还在等状态回传的命令
const CMD_ACK_TIMEOUT = 3000; // 超过这个时间还没等到回传就判定为未确认

function recordCmd(label, bit, want) {
  const entry = { label, ts: Date.now(), state: 'pending', ms: 0 };
  cmdLog.unshift(entry);
  if (cmdLog.length > 12) cmdLog.pop();
  if (bit !== null) {
    pendingCmds.push({ entry, bit, want, ts: entry.ts });
    /* 超时必须由独立定时器触发，不能只在"收到下一帧状态"时顺带检查。
       设备彻底离线时压根不会再有帧过来——而那正是这个功能最该抓住的场景：
       靠帧驱动的话，命令会永远停在"等待回执"，看起来像还在路上，
       实际上是石沉大海。判超时的逻辑不能依赖被判定对象还活着。 */
    setTimeout(expireCmdAcks, CMD_ACK_TIMEOUT + 50);
  } else {
    /* 没有对应状态位的命令（比如调速）只能记成"已发送"。
       诚实标注比假装确认了强——用户需要知道哪些是真确认、哪些只是发出去了 */
    entry.state = 'sent';
  }
  renderCmdLog();
}

/* 每帧状态到达时调用，看看有没有等待中的命令被兑现 */
function checkCmdAcks(stateBits) {
  const now = Date.now();
  let changed = false;
  for (let i = pendingCmds.length - 1; i >= 0; i--) {
    const p = pendingCmds[i];
    if (((stateBits & p.bit) !== 0) === p.want) {
      p.entry.state = 'ok';
      p.entry.ms = now - p.ts;
      pendingCmds.splice(i, 1);
      changed = true;
    }
  }
  if (changed) renderCmdLog();
}

/* 由定时器驱动的超时判定，跟有没有新数据帧无关（见 recordCmd 里的说明） */
function expireCmdAcks() {
  const now = Date.now();
  let changed = false;
  for (let i = pendingCmds.length - 1; i >= 0; i--) {
    const p = pendingCmds[i];
    if (now - p.ts < CMD_ACK_TIMEOUT) continue;
    p.entry.state = 'fail';
    p.entry.ms = now - p.ts;
    pendingCmds.splice(i, 1);
    changed = true;
    /* 告警 key 带上命令内容：不同的命令失败应该是不同的告警条目，
       共用一个 key 的话后一条会把前一条顶掉，只剩最后一次失败 */
    raiseAlert('cmdack_' + p.entry.label, 'warn', '命令未生效',
      p.entry.label + ' 已下发，但 ' + (CMD_ACK_TIMEOUT / 1000) +
      ' 秒内没有等到设备回传对应状态。检查设备是否在线、网关和 CAN 链路是否正常');
  }
  if (changed) renderCmdLog();
}

function renderCmdLog() {
  const box = document.getElementById('cmdLog');
  if (!box) return;
  if (cmdLog.length === 0) {
    box.innerHTML = '<div class="empty-hint">还没有下发过命令</div>';
    return;
  }
  const TXT = {
    pending: ['等待设备回执…', 'pending'],
    ok:      ['已确认', 'ok'],
    fail:    ['未生效', 'fail'],
    sent:    ['已发送（无状态回读）', 'sent'],
  };
  box.innerHTML = cmdLog.map(e => {
    const [txt, cls] = TXT[e.state] || TXT.sent;
    const time = new Date(e.ts).toLocaleTimeString('zh-CN', { hour12: false });
    const ms = (e.state === 'ok') ? ('<em>' + e.ms + ' ms</em>') : '';
    return '<div class="cmd-row ' + cls + '">' +
             '<span class="tm">' + time + '</span>' +
             /* 当前 label 都是代码里的常量，转义只是保持一致——
                但正因为"这里现在是安全的"，才更容易被后来者当成模板抄走 */
             '<span class="lb">' + esc(e.label) + '</span>' +
             '<span class="st">' + txt + ms + '</span>' +
           '</div>';
  }).join('');
}

function pubCmd(d, a, p1, p2) {
  if (!client || !client.connected) { log('未登录，无法下发命令'); return; }
  if (!currentDevice) { log('未选择设备，命令未下发'); return; }
  /* 命令发到指定设备的 topic，不是广播。老协议用全局 monitor/cmd，
     多台机器人在线时按一次"前进"会让所有车一起动，这在真实场景里是事故 */
  if (!deviceOnline) {
    log('设备 ' + deviceLabel(currentDevice) + ' 当前离线，命令可能不会被执行');
  }
  const msg = JSON.stringify({ d, a, p1, p2 });
  client.publish('monitor/' + currentDevice + '/cmd', msg);
  log('下发到 ' + deviceLabel(currentDevice) + ': dev=' + d + ' act=' + a + ' p1=' + p1);
}
function toggleDev(dev) {
  if (dev === 1) {
    ledState.on = !ledState.on;
    pubCmd(1, ledState.on ? 1 : 2, 0, 0);
    recordCmd('LED ' + (ledState.on ? '开' : '关'), 0x01, ledState.on);
  }
  if (dev === 2) {
    buzzerState.on = !buzzerState.on;
    pubCmd(2, buzzerState.on ? 1 : 2, 0, 0);
    recordCmd('蜂鸣器 ' + (buzzerState.on ? '开' : '关'), 0x02, buzzerState.on);
  }
}
const CAR_ACT_NAME = { 1:'前进', 2:'停止', 3:'后退', 4:'左转', 5:'右转', 6:'调速' };
function carCmd(act) {
  const speed = parseInt(document.getElementById('speed').value);
  pubCmd(3, act, speed, (act === 4 || act === 5) ? 1000 : 0);
  /* 电机状态位（bit2）只表示"在转/没转"，转向和调速反映不到位上，
     所以只有"前进/后退/停止"能做闭环确认，其余标成"已发送" */
  const bit = (act === 1 || act === 3) ? 0x04 : (act === 2 ? 0x04 : null);
  const want = (act !== 2);
  recordCmd('小车 ' + (CAR_ACT_NAME[act] || act), bit, bit === null ? false : want);
}
/* 自定义的文件选择按钮没法像原生控件那样自动显示已选文件名，得自己回显。
   不显示的话用户点完"选择固件文件…"完全看不出选中了什么，只能凭记忆 */
const fwFilesEl = document.getElementById('fwFiles');
if (fwFilesEl) {
  fwFilesEl.addEventListener('change', () => {
    const el = document.getElementById('fwFileName');
    const n = fwFilesEl.files.length;
    if (!el) return;
    if (n === 0) { el.textContent = '未选择'; el.classList.remove('has'); return; }
    el.textContent = n === 1
      ? fwFilesEl.files[0].name
      : (fwFilesEl.files[0].name + ' 等 ' + n + ' 个文件');
    el.classList.add('has');
  });
}

const speedEl = document.getElementById('speed');
speedEl.oninput = () => { document.getElementById('speedVal').textContent = speedEl.value + '%'; };

// ============================================================
// 实时曲线（温度/湿度，姿态角）
// ============================================================
const MAX_PTS = 60;
const SAMPLE_MS = 1000;   // STM32 status 上报周期，只用来给趋势图标时间轴刻度
/* 趋势图画温度 + 湿度：这两个是同一类环境量、变化尺度也接近，放一张图上
   对照才有意义。原来配的是"温度 + 距离"——距离是超声波避障用的，和温度
   没有任何相关性，两条线放一起纯粹是凑数，看不出任何信息 */
const tempData = [], humiData = [], distData = [];
const chart = document.getElementById('chart');
let chartHover = -1;
function pushChart(t, h, d) {
  tempData.push(t); humiData.push(h); distData.push(d);
  if (tempData.length > MAX_PTS) { tempData.shift(); humiData.shift(); distData.shift(); }
  if (document.getElementById('page-history').classList.contains('active')) drawChart();
  if (document.getElementById('page-dashboard').classList.contains('active')) { drawDvChart(); updateEnvRange(); }
}

// ============================================================
// 图表引擎
// 之前那版折线只是"把点连起来"：没有刻度、没有单位、没有当前值，
// 看得见趋势但读不出数值。这里重写成带坐标系的图表——左侧 Y 轴刻度、
// 底部时间轴、渐变面积、最新点高亮标注，鼠标移上去出十字线和数值。
// 两条曲线量纲不同（℃ 和 cm），各自独立归一化，各用一侧 Y 轴。
// ============================================================
const PAD = { l: 34, r: 40, t: 10, b: 18 };

function fitCanvas(cv) {
  const W = cv.clientWidth, H = cv.clientHeight;
  if (W < 2 || H < 2) return null;
  cv.width = W * devicePixelRatio; cv.height = H * devicePixelRatio;
  const c = cv.getContext('2d');
  c.setTransform(1, 0, 0, 1, 0, 0); c.scale(devicePixelRatio, devicePixelRatio);
  c.clearRect(0, 0, W, H);
  return { c, W, H };
}

// 数据点 -> 画布坐标
function plotX(i, n, W) { return PAD.l + (i / Math.max(n - 1, 1)) * (W - PAD.l - PAD.r); }
function plotY(v, min, max, H) {
  const r = (v - min) / (max - min || 1);
  return H - PAD.b - r * (H - PAD.t - PAD.b);
}

// 给定数值范围，取一个好看的刻度区间（1/2/5 × 10^n）
function niceStep(range, target) {
  const raw = range / target, p = Math.pow(10, Math.floor(Math.log10(raw))), n = raw / p;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * p;
}
// 数据的实际范围向外扩一点取整，避免曲线贴着上下边缘
function axisRange(arr, fallbackMin, fallbackMax) {
  if (!arr.length) return [fallbackMin, fallbackMax];
  let lo = Math.min(...arr), hi = Math.max(...arr);
  if (hi - lo < 1e-6) { lo -= 1; hi += 1; }
  const pad = (hi - lo) * 0.15;
  return [lo - pad, hi + pad];
}

function smoothPath(c, pts) {
  c.beginPath();
  c.moveTo(pts[0].x, pts[0].y);
  for (let i = 0; i < pts.length - 1; i++) {
    const a = pts[i], b = pts[i + 1], mx = (a.x + b.x) / 2;
    c.bezierCurveTo(mx, a.y, mx, b.y, b.x, b.y);   // 水平中点做控制点，曲线平滑但不过冲
  }
}

// 画一条带面积填充的曲线，并在右端标出当前值
/* 图表配色随主题走。
 *
 * 原来这些颜色全是写死的暗色值——线用 #35d6ff、网格 rgba(77,159,255,.10)、
 * 坐标文字 rgba(138,176,220,.75)。切到日间模式之后：
 *   网格 10% 的淡蓝铺在白底上等于没有，图表失去参照；
 *   坐标文字和线条都太淡，读数要凑近看。
 *
 * CSS 变量管不到 canvas —— canvas 里画什么颜色是 JS 一笔笔指定的，
 * 这是"换主题"最容易漏掉的一块：DOM 部分早就变了，图表还留在旧配色里。
 *
 * 两套值的取法：暗色用亮色在深底上发光；浅色反过来，用足够深的颜色
 * 压在白底上，并把网格的透明度提上来（浅底上同样的透明度看起来更淡）。
 */
function chartTheme() {
  const light = document.body.getAttribute('data-theme') === 'light';
  return light ? {
    temp:  '#0b6bcb',
    humi:  '#b45309',
    grid:  'rgba(17,24,39,.09)',
    axis:  'rgba(17,24,39,.22)',
    label: 'rgba(75,85,99,.95)',
    cross: 'rgba(17,24,39,.35)',
    tipBg: 'rgba(255,255,255,.97)',
    tipBd: 'rgba(11,107,203,.45)',
    err:   'rgba(200,30,58,.9)',
    hi:    '#c81e3a', lo: '#0b6bcb',
    glow:  0            /* 白底上再加辉光只会糊成一团 */
  } : {
    temp:  '#35d6ff',
    humi:  '#ffab3d',
    grid:  'rgba(77,159,255,.10)',
    axis:  'rgba(77,159,255,.35)',
    label: 'rgba(138,176,220,.75)',
    cross: 'rgba(200,225,255,.5)',
    tipBg: 'rgba(6,18,38,.94)',
    tipBd: 'rgba(77,159,255,.5)',
    err:   'rgba(255,138,138,.85)',
    hi:    '#ff4d6d', lo: '#4d9fff',
    glow:  6
  };
}

function drawSeries(c, arr, min, max, W, H, color, unit, side) {
  if (arr.length < 2) return;
  const pts = arr.map((v, i) => ({ x: plotX(i, arr.length, W), y: plotY(v, min, max, H) }));

  const grad = c.createLinearGradient(0, PAD.t, 0, H - PAD.b);
  grad.addColorStop(0, color + '44');
  grad.addColorStop(1, color + '03');
  smoothPath(c, pts);
  c.lineTo(pts[pts.length - 1].x, H - PAD.b);
  c.lineTo(pts[0].x, H - PAD.b);
  c.closePath();
  c.fillStyle = grad; c.fill();

  smoothPath(c, pts);
  c.strokeStyle = color; c.lineWidth = 1.8;
  c.shadowColor = color; c.shadowBlur = chartTheme().glow;
  c.stroke();
  c.shadowBlur = 0;

  // 最新点：实心点 + 外发光圈 + 数值气泡，一眼能读出"现在是多少"
  const last = pts[pts.length - 1], lastV = arr[arr.length - 1];
  c.beginPath(); c.arc(last.x, last.y, 5.5, 0, Math.PI * 2);
  c.fillStyle = color + '33'; c.fill();
  c.beginPath(); c.arc(last.x, last.y, 2.6, 0, Math.PI * 2);
  c.fillStyle = color; c.fill();

  const txt = lastV.toFixed(1) + unit;
  c.font = '600 11px Bahnschrift, Consolas, monospace';
  const tw = c.measureText(txt).width;
  // 两条曲线的最新点可能挨得很近，气泡一上一下错开，避免叠在一起
  const bx = Math.min(last.x + 7, W - tw - 8);
  const by = side === 'left' ? last.y - 8 : last.y + 16;
  c.fillStyle = chartTheme().tipBg;
  c.fillRect(bx - 3, by - 9, tw + 6, 14);
  c.strokeStyle = color + '88'; c.lineWidth = 1;
  c.strokeRect(bx - 3, by - 9, tw + 6, 14);
  c.fillStyle = color;
  c.textAlign = 'left'; c.textBaseline = 'middle';
  c.fillText(txt, bx, by - 2);

  // Y 轴刻度标签：左轴给第一条曲线，右轴给第二条
  const step = niceStep(max - min, 3);
  c.font = '10px Bahnschrift, Consolas, monospace';
  c.fillStyle = color + 'bb';
  c.textAlign = side === 'left' ? 'right' : 'left';
  for (let v = Math.ceil(min / step) * step; v <= max; v += step) {
    const y = plotY(v, min, max, H);
    if (y < PAD.t || y > H - PAD.b) continue;
    c.fillText(String(+v.toFixed(1)), side === 'left' ? PAD.l - 5 : W - PAD.r + 5, y);
  }
}

const dvChart = document.getElementById('dvChart');
let dvHover = -1;   // 鼠标所在的数据点下标，-1 表示没有悬停

function drawDvChart() { renderTrendChart(dvChart, dvHover); }

// 温度/距离趋势图的渲染主体。大屏和历史数据页共用，避免两处各写一份
// 导致风格又跑偏——之前历史页那张还是"只连线不标数"的老样子
function renderTrendChart(canvas, hoverIdx) {
  const f = fitCanvas(canvas);
  if (!f) return;
  const { c, W, H } = f;
  const [tLo, tHi] = axisRange(tempData, 20, 30);
  const [hLo, hHi] = axisRange(humiData, 0, 100);

  // 网格：横线跟左轴刻度对齐，竖线按时间等分
  c.strokeStyle = chartTheme().grid; c.lineWidth = 1;
  const step = niceStep(tHi - tLo, 3);
  for (let v = Math.ceil(tLo / step) * step; v <= tHi; v += step) {
    const y = plotY(v, tLo, tHi, H);
    if (y < PAD.t || y > H - PAD.b) continue;
    c.beginPath(); c.moveTo(PAD.l, y); c.lineTo(W - PAD.r, y); c.stroke();
  }
  for (let i = 0; i <= 4; i++) {
    const x = PAD.l + (i / 4) * (W - PAD.l - PAD.r);
    c.beginPath(); c.moveTo(x, PAD.t); c.lineTo(x, H - PAD.b); c.stroke();
  }
  // 坐标轴
  c.strokeStyle = chartTheme().axis;
  c.beginPath(); c.moveTo(PAD.l, PAD.t); c.lineTo(PAD.l, H - PAD.b); c.lineTo(W - PAD.r, H - PAD.b); c.stroke();

  // 时间轴：最右是"现在"，往左推算每个采样点的时刻
  c.font = '10px Bahnschrift, Consolas, monospace';
  c.fillStyle = chartTheme().label;
  c.textAlign = 'center'; c.textBaseline = 'top';
  const n = tempData.length;
  if (n > 1) {
    for (let i = 0; i <= 4; i++) {
      const idx = Math.round((i / 4) * (n - 1));
      const ago = (n - 1 - idx) * SAMPLE_MS;
      // 窗口只有 60 秒，标到分钟的话五个刻度全是同一个值，要标到秒
      const t = new Date(Date.now() - ago);
      c.fillText(String(t.getMinutes()).padStart(2,'0') + ':' + String(t.getSeconds()).padStart(2,'0'),
                 plotX(idx, n, W), H - PAD.b + 4);
    }
  }

  drawSeries(c, tempData, tLo, tHi, W, H, chartTheme().temp, '℃', 'left');
  drawSeries(c, humiData, hLo, hHi, W, H, chartTheme().humi, '%', 'right');

  // 悬停十字线 + 该时刻两个数值
  if (hoverIdx >= 0 && hoverIdx < n) {
    const x = plotX(hoverIdx, n, W);
    c.strokeStyle = chartTheme().cross; c.setLineDash([3, 3]);
    c.beginPath(); c.moveTo(x, PAD.t); c.lineTo(x, H - PAD.b); c.stroke();
    c.setLineDash([]);
    const lines = ['温度 ' + tempData[hoverIdx].toFixed(1) + '℃', '湿度 ' + humiData[hoverIdx].toFixed(1) + '%'];
    c.font = '11px Bahnschrift, Consolas, monospace';
    const bw = Math.max(...lines.map(s => c.measureText(s).width)) + 12;
    const bx = Math.min(x + 8, W - bw - 4);
    c.fillStyle = chartTheme().tipBg;
    c.fillRect(bx, PAD.t + 2, bw, 32);
    c.strokeStyle = chartTheme().tipBd; c.strokeRect(bx, PAD.t + 2, bw, 32);
    c.textAlign = 'left'; c.textBaseline = 'top';
    c.fillStyle = chartTheme().temp; c.fillText(lines[0], bx + 6, PAD.t + 6);
    c.fillStyle = chartTheme().humi; c.fillText(lines[1], bx + 6, PAD.t + 19);
  }
}

// ============================================================
// 历史回看：数据来自服务器侧 history_logger（订阅 MQTT 落 SQLite）
// history_logger 跟 broker 同机部署，所以直接用 broker 地址 + 8090。
// 跟实时曲线的区别：实时曲线只有内存里那 60 个点，刷新就没了；
// 这里能按小时/天回看，断线期间的数据也在（只要设备当时在线）
// ============================================================
const HIST_PORT = 8090;
const HIST_UNITS = {
  temp:'℃', humi:'%', light:'%', dist:'cm',
  pitch:'°', roll:'°', yaw:'°', rpm_l:'RPM', rpm_r:'RPM'
};
let histPoints = [];
let histHover = -1;

function histApi(path) {
  return 'http://' + (brokerHost || location.hostname) + ':' + HIST_PORT + path;
}

/* 带超时的 fetch。浏览器的 fetch 本身没有超时，服务器不可达时会一直等到
   TCP 超时（可能 70 秒以上），这期间界面就干挂在"加载中"上，用户既不知道
   在等什么、也不知道要等多久。历史服务没起来是部署时很常见的情况，
   必须尽快给出明确反馈 */
async function fetchWithTimeout(url, ms, opts) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), ms || 8000);
  try {
    return await fetch(url, Object.assign({}, opts || {}, { signal: ctl.signal }));
  } catch (e) {
    if (e.name === 'AbortError') throw new Error('请求超时（服务无响应）');
    throw new Error('无法连接（服务未启动或地址不通）');
  } finally {
    clearTimeout(timer);
  }
}

/* 历史图当前处于哪种状态。画布上的提示语必须跟真实状态对上：
   原来无论"正在加载""服务连不上""这段时间确实没数据"，画布上一律写
   "暂无历史数据"——最要命的是第二种，明明是服务没起来，却告诉用户
   "没有数据"，把人往"是不是设备当时没在线"的错误方向带。 */
let histState = 'empty';   // loading | error | empty | ok

async function loadHistory() {
  const metric = document.getElementById('histMetric').value;
  const hours = document.getElementById('histRange').value;
  const statEl = document.getElementById('histStat');
  statEl.textContent = '加载中…';
  histState = 'loading';
  histPoints = [];
  drawHistChart();
  try {
    const r = await fetchWithTimeout(histApi('/api/history?metric=' + metric + '&hours=' + hours + '&limit=3000'), 8000);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    histPoints = j.points || [];
    histState = histPoints.length === 0 ? 'empty' : 'ok';
    if (histPoints.length === 0) {
      statEl.textContent = '这段时间没有数据（历史服务刚启动？设备当时离线？）';
    } else {
      const span = (histPoints[histPoints.length-1].ts - histPoints[0].ts) / 60000;
      statEl.textContent = histPoints.length + ' 个点，跨度 ' +
        (span >= 60 ? (span/60).toFixed(1) + ' 小时' : span.toFixed(0) + ' 分钟');
    }
    drawHistChart();
  } catch (e) {
    // 最常见的原因是历史服务没起来，或者浏览器和它跨域/跨主机不通，
    // 直接把排查方向写在界面上，省得对着空白图猜
    statEl.textContent = '读取失败：' + e.message +
      '（确认服务器上 edgemonitor-history 在跑、' + histApi('') + ' 可达）';
    histPoints = [];
    histState = 'error';
    drawHistChart();
  }
}

// 历史图跟实时图不能共用渲染：这里只有一条曲线，X 轴跨度是几小时到几天，
// 时间刻度得按跨度选格式（分钟级显示 时:分，跨天显示 月/日）
function drawHistChart() {
  const cv = document.getElementById('histChart');
  const f = fitCanvas(cv);
  if (!f) return;
  const { c, W, H } = f;
  const metric = document.getElementById('histMetric').value;
  const unit = HIST_UNITS[metric] || '';

  if (histPoints.length < 2) {
    const TIP = {
      loading: ['正在读取历史数据…', ''],
      error:   ['无法连接历史服务', '检查服务器上的 history_logger 是否在运行'],
      empty:   ['这段时间没有记录', '换个时间范围，或确认设备当时是否在线'],
      ok:      ['数据点不足以绘制曲线', '至少需要两个采样点'],
    }[histState] || ['暂无历史数据', ''];
    c.textAlign = 'center'; c.textBaseline = 'middle';
    c.font = '600 13px "Segoe UI", sans-serif';
    c.fillStyle = histState === 'error' ? chartTheme().err : chartTheme().label;
    c.fillText(TIP[0], W / 2, H / 2 - (TIP[1] ? 9 : 0));
    if (TIP[1]) {
      c.font = '11px "Segoe UI", sans-serif';
      c.fillStyle = chartTheme().label;
      c.fillText(TIP[1], W / 2, H / 2 + 11);
    }
    return;
  }

  const vals = histPoints.map(p => p.v);
  const [lo, hi] = axisRange(vals, 0, 1);
  const t0 = histPoints[0].ts, t1 = histPoints[histPoints.length - 1].ts;
  const spanH = (t1 - t0) / 3600000;
  const xAt = ts => PAD.l + ((ts - t0) / Math.max(t1 - t0, 1)) * (W - PAD.l - PAD.r);

  // 网格 + Y 轴刻度
  c.strokeStyle = chartTheme().grid; c.lineWidth = 1;
  c.font = '10px Bahnschrift, Consolas, monospace';
  c.textAlign = 'right'; c.textBaseline = 'middle';
  const step = niceStep(hi - lo, 4);
  for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
    const y = plotY(v, lo, hi, H);
    if (y < PAD.t || y > H - PAD.b) continue;
    c.beginPath(); c.moveTo(PAD.l, y); c.lineTo(W - PAD.r, y); c.stroke();
    c.fillStyle = chartTheme().temp;
    c.fillText(String(+v.toFixed(1)), PAD.l - 5, y);
  }
  c.strokeStyle = chartTheme().axis;
  c.beginPath(); c.moveTo(PAD.l, PAD.t); c.lineTo(PAD.l, H - PAD.b); c.lineTo(W - PAD.r, H - PAD.b); c.stroke();

  // X 轴时间刻度，格式随跨度变
  c.textAlign = 'center'; c.textBaseline = 'top';
  c.fillStyle = chartTheme().label;
  for (let i = 0; i <= 5; i++) {
    const ts = t0 + (i / 5) * (t1 - t0);
    const d = new Date(ts);
    const label = spanH > 26
      ? (d.getMonth() + 1) + '/' + d.getDate()
      : String(d.getHours()).padStart(2,'0') + ':' + String(d.getMinutes()).padStart(2,'0');
    c.fillText(label, xAt(ts), H - PAD.b + 4);
  }

  // 面积 + 曲线
  const pts = histPoints.map(p => ({ x: xAt(p.ts), y: plotY(p.v, lo, hi, H) }));
  const grad = c.createLinearGradient(0, PAD.t, 0, H - PAD.b);
  grad.addColorStop(0, chartTheme().temp + '44'); grad.addColorStop(1, chartTheme().temp + '03');
  smoothPath(c, pts);
  c.lineTo(pts[pts.length-1].x, H - PAD.b); c.lineTo(pts[0].x, H - PAD.b); c.closePath();
  c.fillStyle = grad; c.fill();
  smoothPath(c, pts);
  c.strokeStyle = chartTheme().temp; c.lineWidth = 1.6;
  c.shadowColor = chartTheme().temp; c.shadowBlur = chartTheme().glow; c.stroke(); c.shadowBlur = 0;

  // 最大/最小值标注：回看一段历史时，最关心的往往就是这两个极值出现在什么时候
  const iMax = vals.indexOf(Math.max(...vals)), iMin = vals.indexOf(Math.min(...vals));
  [[iMax, chartTheme().hi, '最高'], [iMin, chartTheme().lo, '最低']].forEach(([i, color, tag]) => {
    const p = pts[i];
    c.beginPath(); c.arc(p.x, p.y, 3, 0, Math.PI * 2); c.fillStyle = color; c.fill();
    const txt = tag + ' ' + vals[i].toFixed(1) + unit;
    c.font = '10px Bahnschrift, Consolas, monospace';
    const tw = c.measureText(txt).width;
    const bx = Math.max(PAD.l, Math.min(p.x - tw / 2, W - PAD.r - tw));
    c.fillStyle = color; c.textAlign = 'left'; c.textBaseline = 'bottom';
    c.fillText(txt, bx, p.y - 6);
  });

  // 悬停读数
  if (histHover >= 0 && histHover < histPoints.length) {
    const p = pts[histHover], d = histPoints[histHover];
    c.strokeStyle = chartTheme().cross; c.setLineDash([3,3]);
    c.beginPath(); c.moveTo(p.x, PAD.t); c.lineTo(p.x, H - PAD.b); c.stroke();
    c.setLineDash([]);
    const dt = new Date(d.ts);
    const lines = [d.v.toFixed(1) + unit,
      dt.toLocaleDateString('zh-CN') + ' ' + dt.toLocaleTimeString('zh-CN', { hour12:false })];
    c.font = '11px Bahnschrift, Consolas, monospace';
    const bw = Math.max(...lines.map(s => c.measureText(s).width)) + 12;
    const bx = Math.min(p.x + 8, W - bw - 4);
    c.fillStyle = chartTheme().tipBg; c.fillRect(bx, PAD.t + 2, bw, 32);
    c.strokeStyle = chartTheme().tipBd; c.strokeRect(bx, PAD.t + 2, bw, 32);
    c.textAlign = 'left'; c.textBaseline = 'top';
    c.fillStyle = chartTheme().temp; c.fillText(lines[0], bx + 6, PAD.t + 6);
    c.fillStyle = chartTheme().label; c.fillText(lines[1], bx + 6, PAD.t + 19);
  }
}

// 历史事件（设备上下线等），跟内存里那份本次会话告警是两回事：
// 这里的记录跨会话、跨浏览器刷新、跨服务重启都还在
async function loadHistoryEvents() {
  const list = document.getElementById('histEventList');
  const stat = document.getElementById('histEvtStat');
  if (!list) return;
  stat.textContent = '加载中…';
  try {
    const r = await fetchWithTimeout(histApi('/api/events?limit=100'), 8000);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    const evs = j.events || [];
    stat.textContent = '共 ' + evs.length + ' 条';
    if (evs.length === 0) {
      list.innerHTML = '<div class="empty-hint">历史库里还没有事件记录</div>';
      return;
    }
    list.innerHTML = evs.map(e => {
      const d = new Date(e.ts);
      /* info 也要带上等级类名：圆点和左侧色条都靠它上色，
         留空的话"设备上线"这类事件会显示成一个没有颜色的灰点，
         看着像是等级未知，而不是"这是一条正常信息" */
      const lv = (e.level === 'danger' || e.level === 'warn') ? e.level : 'info';
      return '<div class="alert-item ' + lv + '">' +
        '<div class="ic"></div>' +
        '<div class="body"><div class="t">' + esc(e.title) + '　<span style="color:var(--text-faint);font-size:11px;">' + esc(e.device) + '</span></div>' +
        '<div class="m">' + esc(e.msg || '') + '</div></div>' +
        '<div class="ts">' + d.toLocaleDateString('zh-CN') + '<br>' +
        d.toLocaleTimeString('zh-CN', { hour12:false }) + '</div></div>';
    }).join('');
  } catch (e) {
    stat.textContent = '读取失败';
    list.innerHTML = '<div class="empty-hint">读取历史事件失败：' + esc(e.message) +
      '<br>确认服务器上 edgemonitor-history 服务在跑</div>';
  }
}

// 历史事件的内容里有设备名和消息体，虽然目前都是自家程序写进去的，
// 但它经过了 broker 和数据库两道转手，直接拼进 innerHTML 不合适
function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

(function bindHistHover() {
  const cv = document.getElementById('histChart');
  if (!cv) return;
  cv.addEventListener('mousemove', (e) => {
    if (histPoints.length < 2) return;
    const r = cv.getBoundingClientRect();
    const t0 = histPoints[0].ts, t1 = histPoints[histPoints.length-1].ts;
    const ratio = (e.clientX - r.left - PAD.l) / (r.width - PAD.l - PAD.r);
    const ts = t0 + Math.max(0, Math.min(1, ratio)) * (t1 - t0);
    // 按时间找最近的点，而不是按下标——历史数据的点在时间上不一定均匀
    // （设备离线那段没有数据，采样间隔也可能变过）
    let best = 0, bestD = Infinity;
    histPoints.forEach((p, i) => {
      const dd = Math.abs(p.ts - ts);
      if (dd < bestD) { bestD = dd; best = i; }
    });
    histHover = best;
    drawHistChart();
  });
  cv.addEventListener('mouseleave', () => { histHover = -1; drawHistChart(); });
})();

// ============================================================
// 远程屏幕：拉取板子 framebuffer 显示 + 点击注入触摸
//
// 板子跑 linuxfb 直出（没有 X server），VNC 那一套无从下手，直接读
// /dev/fb0 是最短路径。传的是 2x2 降采样后的原始 RGB565——板子上不一定
// 有 libjpeg，这样零依赖，代价是带宽大些（512x300 约 300KB/帧），
// 所以帧率给到 1~2fps 就够：这个功能是"远程看一眼、点两下"，不是看视频。
// ============================================================
const SCREEN_PORT = 8082;
let scrTimer = null;
let scrBusy = false;          // 上一帧还没回来就不发新请求，避免慢网络下请求堆积
let scrScreenW = 0, scrScreenH = 0;   // 板子屏幕真实分辨率，点击换算要用
let scrFrames = 0, scrErrs = 0, scrLastBytes = 0;
/* 滚轮累积量。滚轮事件触发得非常密（一次物理滚动能出十几个 event），
   每个都发一次滑动请求会把板子和网络都打满，所以先攒一小段再合成一次滑动 */
let scrWheelAccum = 0, scrWheelTimer = null;

function screenApi(path) {
  const d = devices[currentDevice];
  const ip = d && d.ip;
  if (!ip) return null;
  return 'http://' + ip + ':' + SCREEN_PORT + path +
         (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(videoToken);
}

function toggleScreenShare() {
  if (scrTimer) stopScreenShare();
  else startScreenShare();
}

function startScreenShare() {
  const url = screenApi('/frame');
  if (!url) {
    document.getElementById('scrStat').textContent = '当前设备没有上报 IP（离线？），无法连接';
    return;
  }
  scrFrames = 0; scrErrs = 0;
  document.getElementById('scrToggle').textContent = '停止查看';
  setScrIdle(false);
  const period = parseInt(document.getElementById('scrFps').value, 10);
  scrTimer = setInterval(pullScreenFrame, period);
  pullScreenFrame();
}

function stopScreenShare() {
  if (scrTimer) { clearInterval(scrTimer); scrTimer = null; }
  document.getElementById('scrToggle').textContent = '开始查看';
  /* 停止后画布上还留着最后一帧，而它跟实时画面【长得一模一样】——
     跟传感器陈旧值是同一类问题：冻结的画面伪装成实时的。
     必须明确盖一层说明，否则有人会对着一张定格的截图判断设备状态。 */
  setScrIdle(true, '已停止刷新', '画面停在最后一帧，不是当前状态。再点「开始查看」恢复实时');
}

/* 远程屏幕的覆盖提示。三种场景共用：还没开始、已停止、拉取失败 */
function setScrIdle(show, title, hint) {
  const el = document.getElementById('scrIdle');
  if (!el) return;
  if (show) {
    if (title) {
      const b = el.querySelector('b'), s = el.querySelector('span');
      if (b) b.textContent = title;
      if (s) s.textContent = hint || '';
    }
    el.classList.remove('hide');
  } else {
    el.classList.add('hide');
  }
}

function restartScreenShare() {
  if (scrTimer) { stopScreenShare(); startScreenShare(); }
}

async function pullScreenFrame() {
  if (scrBusy) return;
  const url = screenApi('/frame');
  if (!url) return;
  scrBusy = true;
  try {
    const r = await fetchWithTimeout(url + '&_=' + Date.now(), 6000);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const fw = parseInt(r.headers.get('X-Frame-Width') || '0', 10);
    const fh = parseInt(r.headers.get('X-Frame-Height') || '0', 10);
    scrScreenW = parseInt(r.headers.get('X-Screen-Width') || '0', 10) || fw;
    scrScreenH = parseInt(r.headers.get('X-Screen-Height') || '0', 10) || fh;
    const cv = document.getElementById('scrCanvas');
    const ctx = cv.getContext('2d');
    const ctype = r.headers.get('Content-Type') || '';
    let bytes = 0;

    if (ctype.indexOf('image/') === 0) {
      /* 板子有 libjpeg 时走这条：全分辨率 JPEG，比降采样的原始数据
         又清楚又小（一帧 40~80KB vs 300KB），解码也交给浏览器原生做 */
      const blob = await r.blob();
      bytes = blob.size;
      const bmp = await createImageBitmap(blob);
      if (cv.width !== bmp.width || cv.height !== bmp.height) {
        cv.width = bmp.width; cv.height = bmp.height;
      }
      ctx.drawImage(bmp, 0, 0);
      bmp.close();
    } else {
      /* 没有 libjpeg 时的兜底：降采样后的原始 RGB565，前端自己转 */
      const buf = new Uint16Array(await r.arrayBuffer());
      bytes = buf.byteLength;
      if (!fw || !fh || buf.length < fw * fh) throw new Error('帧数据不完整');
      if (cv.width !== fw || cv.height !== fh) { cv.width = fw; cv.height = fh; }
      const img = ctx.createImageData(fw, fh);
      const px = img.data;
      /* RGB565 -> RGBA8888。低位补高位（<<3 | >>2）而不是直接补零，
         否则纯白 0xFFFF 会变成 248,252,248 这种发灰的白 */
      for (let i = 0, j = 0; i < fw * fh; i++, j += 4) {
        const v = buf[i];
        const r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
        px[j]     = (r5 << 3) | (r5 >> 2);
        px[j + 1] = (g6 << 2) | (g6 >> 4);
        px[j + 2] = (b5 << 3) | (b5 >> 2);
        px[j + 3] = 255;
      }
      ctx.putImageData(img, 0, 0);
    }
    scrLastBytes = bytes;
    scrFrames++;
    document.getElementById('scrStat').textContent =
      scrFrames + ' 帧 · ' + cv.width + '×' + cv.height +
      ' · ' + Math.round(scrLastBytes / 1024) + ' KB/帧' +
      (scrErrs ? ' · 失败 ' + scrErrs + ' 次' : '');
  } catch (e) {
    scrErrs++;
    document.getElementById('scrStat').textContent =
      '读取失败：' + e.message + '（确认板子上 screen_share 在跑、' +
      (screenApi('') || '').split('?')[0] + ' 可达）';
    // 连续失败就别再刷了，省得一直报错刷屏
    if (scrErrs >= 5) { stopScreenShare(); }
  } finally {
    scrBusy = false;
  }
}

/* 把画面上的鼠标位置换算成板子屏幕的真实像素。
   要经过两层缩放：CSS 显示尺寸 → 画面像素 → 屏幕像素。
   canvas 会被 CSS 拉伸、画面本身可能还是降采样的，少算一层点击位置就偏 */
function screenPointFromEvent(ev) {
  const rect = ev.currentTarget.getBoundingClientRect();
  const cx = (ev.clientX - rect.left) / rect.width;
  const cy = (ev.clientY - rect.top) / rect.height;
  return {
    x: Math.round(Math.max(0, Math.min(1, cx)) * scrScreenW),
    y: Math.round(Math.max(0, Math.min(1, cy)) * scrScreenH)
  };
}

async function sendTouch(query, desc) {
  const url = screenApi('/touch?' + query);
  if (!url) return;
  try {
    const r = await fetchWithTimeout(url, 4000);
    document.getElementById('scrStat').textContent = r.ok ? desc : (desc + ' 失败：HTTP ' + r.status);
    // 操作完立刻抓一帧，让界面变化尽快显示，不用干等下一个定时周期
    setTimeout(pullScreenFrame, 150);
  } catch (e) {
    document.getElementById('scrStat').textContent = desc + ' 失败：' + e.message;
  }
}

async function onScreenClick(ev) {
  if (!document.getElementById('scrClickable').checked) return;
  if (!scrScreenW || !scrScreenH) return;
  const p = screenPointFromEvent(ev);
  sendTouch('x=' + p.x + '&y=' + p.y, '已点击 (' + p.x + ', ' + p.y + ')');
}

/* 滚轮 → 设备上的滑动。
   必须 preventDefault：不然鼠标在画面上滚时，浏览器不知道你是想滚网页
   还是想滚板子的界面，两边一起动，很难受。既然指针已经在画面里了，
   意图就是操作设备，页面滚动交给画面外的区域 */
function onScreenWheel(ev) {
  if (!document.getElementById('scrClickable').checked) return;
  ev.preventDefault();
  if (!scrScreenW || !scrScreenH) return;

  scrWheelAccum += ev.deltaY;
  if (scrWheelTimer) return;
  const p = screenPointFromEvent(ev);

  scrWheelTimer = setTimeout(() => {
    /* 滚轮向下(deltaY>0) = 想看下面的内容 = 手指从下往上划，所以 y 要减。
       幅度限制在 ±260px，一次滑太长容易被识别成甩动惯性 */
    let dy = -scrWheelAccum * 1.6;
    dy = Math.max(-260, Math.min(260, dy));
    scrWheelAccum = 0;
    scrWheelTimer = null;
    if (Math.abs(dy) < 12) return;   // 太小的滚动忽略，避免误触
    const y2 = Math.round(Math.max(0, Math.min(scrScreenH - 1, p.y + dy)));
    sendTouch('x=' + p.x + '&y=' + p.y + '&x2=' + p.x + '&y2=' + y2,
              '已滑动 ' + (dy < 0 ? '↑' : '↓') + ' ' + Math.abs(Math.round(dy)) + 'px');
  }, 120);
}

(function bindScreenWheel() {
  const cv = document.getElementById('scrCanvas');
  // passive:false 才允许 preventDefault
  if (cv) cv.addEventListener('wheel', onScreenWheel, { passive: false });
})();

// ---- KPI 卡片下挂的迷你柱状图 ----
// 每个 KPI 记一段自己的历史，画成小柱子。数字告诉你"现在多少"，
// 柱子告诉你"最近怎么变的"，两者加起来才算直观
const sparkHist = { Uptime: [], Frames: [], Alerts: [], Health: [] };
const SPARK_N = 24;

function pushSpark(key, v) {
  const a = sparkHist[key];
  a.push(v);
  if (a.length > SPARK_N) a.shift();
}

function drawSpark(id, key, color) {
  const cv = document.getElementById(id);
  if (!cv) return;
  const f = fitCanvas(cv);
  if (!f) return;
  const { c, W, H } = f;
  const a = sparkHist[key];
  if (!a.length) return;
  const hi = Math.max(...a, 1), lo = Math.min(...a, 0);
  const bw = W / SPARK_N;
  a.forEach((v, i) => {
    const h = Math.max(1, ((v - lo) / (hi - lo || 1)) * (H - 2));
    const x = i * bw;
    // 最新一根高亮，其余压暗，形成"当前值"的视觉锚点
    c.fillStyle = (i === a.length - 1) ? color : color + '55';
    c.fillRect(x, H - h, Math.max(1, bw - 1.5), h);
  });
}

function updateSparks() {
  drawSpark('sparkUptime', 'Uptime', '#4d9fff');
  drawSpark('sparkFrames', 'Frames', '#35d6ff');
  drawSpark('sparkAlerts', 'Alerts', '#ffab3d');
  drawSpark('sparkHealth', 'Health', '#3ddc97');
}

dvChart.addEventListener('mousemove', (e) => {
  const r = dvChart.getBoundingClientRect(), n = tempData.length;
  if (n < 2) return;
  const ratio = (e.clientX - r.left - PAD.l) / (r.width - PAD.l - PAD.r);
  dvHover = Math.round(Math.max(0, Math.min(1, ratio)) * (n - 1));
  drawDvChart();
});
dvChart.addEventListener('mouseleave', () => { dvHover = -1; drawDvChart(); });

/* 环境面板下面原来挂了"温度区间/距离区间"两行极值，已去掉——圆环上已经
   有当前值、趋势图上也能看出波动范围，再列一遍是重复信息。
   函数保留成空实现，免得改动所有调用点 */
function updateEnvRange() {}
// 历史页那张图跟大屏走同一套渲染，连悬停读数都一样
function drawChart() { renderTrendChart(chart, chartHover); }
chart.addEventListener('mousemove', (e) => {
  const r = chart.getBoundingClientRect(), n = tempData.length;
  if (n < 2) return;
  const ratio = (e.clientX - r.left - PAD.l) / (r.width - PAD.l - PAD.r);
  chartHover = Math.round(Math.max(0, Math.min(1, ratio)) * (n - 1));
  drawChart();
});
chart.addEventListener('mouseleave', () => { chartHover = -1; drawChart(); });

const pitchData = [], rollData = [], yawData = [];
const chartImu = document.getElementById('chartImu');
function pushImuChart(p, r, y) {
  pitchData.push(p); rollData.push(r); yawData.push(y);
  if (pitchData.length > MAX_PTS) { pitchData.shift(); rollData.shift(); yawData.shift(); }
  if (document.getElementById('page-history').classList.contains('active')) drawImuChart();
}
/* 姿态角图。原来是一条裸折线：没有纵轴刻度、没有当前值，只能看出"在动"，
   却读不出此刻朝向多少度——正是"不知道现在的方位"这个问题。
   三条线量纲不同（俯仰/横滚是 ±90°，偏航是 ±180°），共用一个纵轴会让
   偏航把另外两条压扁，所以固定用 ±180 做刻度、三条线各自标出当前值。 */
function drawImuChart() {
  const f = fitCanvas(chartImu);
  if (!f) return;
  const { c, W, H } = f;

  const LO = -180, HI = 180;

  // 网格 + 纵轴刻度（每 90°一条，0° 那条加粗——它是水平基准线）
  c.font = '10px Bahnschrift, Consolas, monospace';
  c.textAlign = 'right'; c.textBaseline = 'middle';
  for (let v = LO; v <= HI; v += 90) {
    const y = plotY(v, LO, HI, H);
    c.strokeStyle = (v === 0) ? 'rgba(77,159,255,.35)' : 'rgba(77,159,255,.10)';
    c.beginPath(); c.moveTo(PAD.l, y); c.lineTo(W - PAD.r, y); c.stroke();
    c.fillStyle = 'rgba(138,176,220,.8)';
    c.fillText(v + '°', PAD.l - 5, y);
  }
  c.strokeStyle = chartTheme().axis;
  c.beginPath(); c.moveTo(PAD.l, PAD.t); c.lineTo(PAD.l, H - PAD.b); c.lineTo(W - PAD.r, H - PAD.b); c.stroke();

  // 时间轴
  c.textAlign = 'center'; c.textBaseline = 'top';
  c.fillStyle = chartTheme().label;
  const n = pitchData.length;
  if (n > 1) {
    for (let i = 0; i <= 4; i++) {
      const idx = Math.round((i / 4) * (n - 1));
      const t = new Date(Date.now() - (n - 1 - idx) * SAMPLE_MS);
      c.fillText(String(t.getMinutes()).padStart(2,'0') + ':' + String(t.getSeconds()).padStart(2,'0'),
                 plotX(idx, n, W), H - PAD.b + 4);
    }
  }

  const series = [
    { data: pitchData, color: '#35d6ff', name: '俯仰' },
    { data: rollData,  color: '#ffab3d', name: '横滚' },
    /* 偏航是【环形量】：转过正北时会从 +180 跳到 -180。这是真实的角度变化，
       不是坏数据，但如果照直连线，画面上会出现一条贯穿全图的垂直线，
       看着像"车在一帧之内掉了个头"。wrap:true 让它在跳变处断开，
       断开比画一条假的连线诚实——曲线本来就不连续。 */
    { data: yawData,   color: '#a98bff', name: '偏航', wrap: true },
  ];

  series.forEach(s => {
    if (s.data.length < 2) return;
    const pts = s.data.map((v, i) => ({ x: plotX(i, s.data.length, W), y: plotY(v, LO, HI, H) }));
    c.strokeStyle = s.color; c.lineWidth = 1.6;
    c.shadowColor = s.color; c.shadowBlur = 4;
    if (s.wrap) {
      /* 相邻两点差超过半个量程（180°），只可能是绕回，不可能是真的转了这么多
         ——采样间隔才几百毫秒。在那里切断，分段画 */
      let seg = [pts[0]];
      for (let i = 1; i < pts.length; i++) {
        if (Math.abs(s.data[i] - s.data[i - 1]) > (HI - LO) / 2) {
          if (seg.length > 1) { smoothPath(c, seg); c.stroke(); }
          seg = [];
        }
        seg.push(pts[i]);
      }
      if (seg.length > 1) { smoothPath(c, seg); c.stroke(); }
    } else {
      smoothPath(c, pts);
      c.stroke();
    }
    c.shadowBlur = 0;
    // 末端画点，配合右侧图例读当前值
    const last = pts[pts.length - 1];
    c.beginPath(); c.arc(last.x, last.y, 2.6, 0, Math.PI * 2);
    c.fillStyle = s.color; c.fill();
  });

  // 图例带当前读数，这样不用悬停也能直接读出此刻姿态
  c.font = '11px Bahnschrift, Consolas, monospace';
  c.textAlign = 'left'; c.textBaseline = 'middle';
  let lx = PAD.l + 6;
  series.forEach(s => {
    const cur = s.data.length ? s.data[s.data.length - 1].toFixed(1) + '°' : '--';
    const txt = s.name + ' ' + cur;
    c.fillStyle = s.color;
    c.fillRect(lx, PAD.t + 5, 8, 2);
    c.fillText(txt, lx + 12, PAD.t + 6);
    lx += c.measureText(txt).width + 30;
  });
}

// ============================================================
// 固件上传（浏览器 → 板子）
//
// 接口挂在 screen_share 上（它已经是个 HTTP 服务了，不再多起一个进程）。
// 传的是原始字节而不是 multipart/form-data：后者要在 C 里解析 boundary、
// 逐段扫描，为传一个文件写那套解析器不划算。
// ============================================================
async function uploadFirmware() {
  const input = document.getElementById('fwFiles');
  const files = [...input.files];
  const stat = document.getElementById('fwStatus');
  const bar = document.getElementById('fwBar');
  const btn = document.getElementById('fwUploadBtn');

  if (!files.length) { stat.textContent = '请先选择固件文件'; return; }
  const bad = files.find(f => !/^[A-Za-z0-9._-]+\.bin$/.test(f.name) || f.name.startsWith('.'));
  if (bad) {
    // 板子侧也会校验，这里先挡一道，省得白传一遍才被拒
    stat.textContent = '文件名不合法：' + bad.name + '（只能是字母数字 . _ -，且以 .bin 结尾）';
    return;
  }

  btn.disabled = true;
  bar.style.width = '0%';
  let done = 0;
  try {
    for (const f of files) {
      stat.textContent = '正在上传 ' + f.name + '（' + Math.round(f.size / 1024) + ' KB）…';
      const url = screenApi('/upload?name=' + encodeURIComponent(f.name));
      if (!url) throw new Error('当前设备没有上报 IP（离线？）');
      const r = await fetchWithTimeout(url, 30000, {
        method: 'POST',
        body: await f.arrayBuffer()
      });
      const txt = await r.text();
      if (!r.ok) throw new Error(f.name + ' 上传失败：' + txt);
      done++;
      bar.style.width = Math.round(done / files.length * 100) + '%';
    }
    stat.textContent = '已上传 ' + done + ' 个文件到设备';
    toast('上传完成', done + ' 个固件已就位', 'success');
    loadFwList();
  } catch (e) {
    stat.textContent = e.message;
  } finally {
    btn.disabled = false;
  }
}

async function loadFwList() {
  const box = document.getElementById('fwList');
  const url = screenApi('/fwlist');
  if (!url) { box.innerHTML = '<div class="empty-hint" style="padding:14px;">当前设备离线，无法读取</div>'; return; }
  try {
    const r = await fetchWithTimeout(url, 8000);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    // 兼容旧接口（只返回文件名数组）和新接口（带 size/mtime 的对象）
    const files = (j.files || []).map(f => typeof f === 'string' ? { name: f } : f);
    if (!files.length) {
      box.innerHTML = '<div class="empty-hint" style="padding:14px;">设备上还没有固件文件</div>';
      return;
    }
    /* 把同名的 _a/_b 归成一组：A/B 模式下升级填的是基名，
       直接把基名列出来、点一下就填进升级框，省得手输出错 */
    const bases = {};
    files.forEach(f => {
      const m = f.name.match(/^(.*)_(a|b)\.bin$/);
      const key = m ? m[1] : f.name;
      const g = bases[key] || (bases[key] = { slots: [], files: [], size: 0, mtime: 0, pair: !!m });
      g.slots.push(m ? m[2] : '-');
      g.files.push(f);          // 留着实际文件名，光显示基名的话看不到自己刚传的是哪个
      g.size += (f.size || 0);
      if (f.mtime && f.mtime > g.mtime) g.mtime = f.mtime;
    });

    const fmtTime = ts => {
      if (!ts) return '';
      const d = new Date(ts * 1000);
      const mins = Math.round((Date.now() - d.getTime()) / 60000);
      // 刚传的用相对时间标出来，一眼能确认"这就是我刚上传的那份"
      const rel = mins < 1 ? '刚刚' : mins < 60 ? mins + ' 分钟前' :
                  d.toLocaleDateString('zh-CN') + ' ' + d.toLocaleTimeString('zh-CN', { hour12:false });
      return ' · ' + rel;
    };

    box.innerHTML = Object.keys(bases).sort().map(b => {
      const g = bases[b];
      const slots = g.slots.sort();
      const complete = slots.includes('a') && slots.includes('b');
      const fresh = g.mtime && (Date.now() / 1000 - g.mtime) < 120;   // 2 分钟内算"刚更新"
      /* 把实际文件逐个列出来（名字 + 大小 + 时间）。只显示归组后的基名时，
         上传完根本看不出自己传的那个文件到底在不在、是不是新的 */
      const detail = g.files.sort((p, q) => p.name.localeCompare(q.name)).map(f => {
        const isNew = f.mtime && (Date.now() / 1000 - f.mtime) < 120;
        return '<div class="fw-file' + (isNew ? ' fresh' : '') + '">' +
          esc(f.name) +
          (f.size ? ' <span>' + Math.round(f.size / 1024) + ' KB</span>' : '') +
          (f.mtime ? ' <span>' + new Date(f.mtime * 1000).toLocaleTimeString('zh-CN', { hour12:false }) + '</span>' : '') +
          (isNew ? ' <b>刚上传</b>' : '') +
        '</div>';
      }).join('');

      return '<div class="dev-row">' +
        '<span class="dot ' + (complete || !g.pair ? 'on' : 'off') + '"></span>' +
        '<div class="info"><div class="nm">' + esc(b) +
          (fresh ? ' <em>刚更新</em>' : '') + '</div>' +
        '<div class="sub">' + (g.pair
            ? ('槽 ' + slots.map(s => s.toUpperCase()).join(' / ') +
               (complete ? '' : '　⚠ 缺少另一个槽的固件，升级到该槽时会失败'))
            : '单文件（非 A/B 命名）') +
          (g.size ? ' · 共 ' + Math.round(g.size / 1024) + ' KB' : '') +
          fmtTime(g.mtime) +
        '</div>' + detail + '</div>' +
        '<div class="ops"><button class="ghost" onclick="document.getElementById(\'otaPath\').value=' +
          "'" + esc(b) + "'" + '">填入升级框</button></div>' +
      '</div>';
    }).join('');
  } catch (e) {
    box.innerHTML = '<div class="empty-hint" style="padding:14px;">读取失败：' + esc(e.message) + '</div>';
  }
}

// ============================================================
// 固件 OTA 升级
// ============================================================
function startOta() {
  if (!client || !client.connected) { log('未登录，无法触发升级'); return; }
  const path = document.getElementById('otaPath').value.trim();
  if (!path) { document.getElementById('otaStatus').textContent = '请先填固件文件名'; return; }
  if (path.includes('/') || path.includes('\\') || path.startsWith('.')) {
    document.getElementById('otaStatus').textContent = '只能填文件名，不能带路径（网关会拒绝）';
    return;
  }
  document.getElementById('otaBtn').disabled = true;
  document.getElementById('otaBar').style.width = '0%';
  const st = document.getElementById('otaStatus');
  st.textContent = '已触发，等待网关响应…'; st.className = 'ota-status';
  document.getElementById('otaLog').innerHTML = '';
  /* 发给【当前选中的】那台设备，不再是全局广播 */
  client.publish('monitor/' + currentDevice + '/ota/cmd', JSON.stringify({ path }));
  otaLog('触发升级：' + path);
  toast('OTA 已触发', '固件：' + path, 'info');
}
function updateOtaProgress(data) {
  if (typeof data.pct === 'number') document.getElementById('otaBar').style.width = data.pct + '%';
  if (data.stage) {
    document.getElementById('otaStatus').textContent = data.stage +
      (typeof data.pct === 'number' ? '（' + data.pct + '%）' : '');
    otaLog(data.stage);
  }
}
function finishOta(data) {
  document.getElementById('otaBtn').disabled = false;
  const st = document.getElementById('otaStatus');
  if (data.ok) {
    document.getElementById('otaBar').style.width = '100%';
    st.textContent = '升级成功：' + (data.msg || ''); st.className = 'ota-status ok';
    toast('OTA 成功', data.msg || '', 'success');
  } else {
    st.textContent = '升级失败：' + (data.msg || ''); st.className = 'ota-status fail';
    toast('OTA 失败', data.msg || '', 'danger');
  }
  otaLog((data.ok ? '[成功] ' : '[失败] ') + (data.msg || ''));
}
function otaLog(msg) {
  const el = document.getElementById('otaLog');
  const line = document.createElement('div');
  line.textContent = '[' + new Date().toLocaleTimeString() + '] ' + msg;
  el.prepend(line);
  while (el.children.length > 40) el.removeChild(el.lastChild);
}

// ============================================================
// GPS 地图（Leaflet）
// ============================================================
let gpsMap, gpsMarker, gpsTrack, gpsPoints = [];
let gpsPlaceholder = null;

/* 没有定位时地图上显示的占位点：南京航空航天大学（明故宫校区，御道街 29 号）。
   RTK 模块还没接好之前，地图空着一片什么也说明不了，摆个已知位置至少
   能看出地图组件、瓦片、缩放都是好的。

   **它必须一眼就能看出是假的。** 一个没有标注的坐标点，看的人默认会
   当成车的实际位置——续航估算那里踩过同样的坑：估算值不标明是估算，
   就会被当实测值用。所以这里用虚线空心圈（和真实定位的实心标记完全不同），
   文字里写明"演示位置"，并且真实定位一到就立刻把它撤掉。 */
/* 坐标来自高德 POI 搜索"南京航空航天大学(将军路校区)，将军路29号"，
   是 **GCJ-02**——和下面用的高德瓦片同一个坐标系，直接画不用转。
   凭记忆写一个坐标的话，偏一两百米是常事，而这是整张图唯一的参照点。 */
const GPS_PLACEHOLDER = [31.938249, 118.792231];
const GPS_PLACEHOLDER_NAME = '南京航空航天大学（将军路校区）';

/* WGS-84 -> GCJ-02。
   GNSS 给的是 WGS-84，高德瓦片是 GCJ-02，国内差 300~500 米。
   不转的话车会画到隔壁街区去，而且看着完全像"定位不准"。
   业界通用的近似实现（官方算法不公开），误差 1~5 米，看位置足够；
   要厘米级精度的地方（路径录制）不走这条转换。 */
function wgs84ToGcj02(lat, lon) {
  if (lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271) return [lat, lon];
  const a = 6378245.0, ee = 0.00669342162296594323, PI = Math.PI;
  const tLat = (x, y) => {
    let r = -100 + 2 * x + 3 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
    r += (20 * Math.sin(6 * x * PI) + 20 * Math.sin(2 * x * PI)) * 2 / 3;
    r += (20 * Math.sin(y * PI) + 40 * Math.sin(y / 3 * PI)) * 2 / 3;
    r += (160 * Math.sin(y / 12 * PI) + 320 * Math.sin(y * PI / 30)) * 2 / 3;
    return r;
  };
  const tLon = (x, y) => {
    let r = 300 + x + 2 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
    r += (20 * Math.sin(6 * x * PI) + 20 * Math.sin(2 * x * PI)) * 2 / 3;
    r += (20 * Math.sin(x * PI) + 40 * Math.sin(x / 3 * PI)) * 2 / 3;
    r += (150 * Math.sin(x / 12 * PI) + 300 * Math.sin(x / 30 * PI)) * 2 / 3;
    return r;
  };
  let dLat = tLat(lon - 105, lat - 35), dLon = tLon(lon - 105, lat - 35);
  const rad = lat / 180 * PI;
  let m = Math.sin(rad); m = 1 - ee * m * m;
  const sm = Math.sqrt(m);
  dLat = (dLat * 180) / ((a * (1 - ee)) / (m * sm) * PI);
  dLon = (dLon * 180) / (a / sm * Math.cos(rad) * PI);
  return [lat + dLat, lon + dLon];
}

function showGpsPlaceholder() {
  if (!gpsMap || !window.L) return;
  if (!gpsPlaceholder) {
    gpsPlaceholder = L.circleMarker(GPS_PLACEHOLDER, {
      radius: 9, color: '#8e8e93', weight: 2, dashArray: '4 3',
      fill: true, fillColor: '#8e8e93', fillOpacity: 0.12
    }).addTo(gpsMap);
    /* 不再在点上挂常驻标签：它压在地图正中，挡住了底下的地名。
       "这是演示位置、不是车的实际位置"已经写在地图下方那行说明里了，
       虚线空心圈本身也和真实定位的实心标记区分得开。 */
  }
  const empty = document.getElementById('mapEmpty');
  if (empty) empty.classList.add('hide');
  gpsMap.invalidateSize();
}

function hideGpsPlaceholder() {
  if (gpsPlaceholder && gpsMap) { gpsMap.removeLayer(gpsPlaceholder); gpsPlaceholder = null; }
}
/* Leaflet 是从 CDN 加载的，而这套系统的实际部署环境很可能【上不了外网】
   （工控现场的内网、只有内网的 VM、板子挂在一个没有出口的 AP 上）。
   那种情况下 window.L 根本不存在，地图和瓦片都出不来——这是必须预期的
   常态，不是异常。所以：
     1. initMap 先探测 L 存不存在，不存在就把地图面板换成一句说明，
        而不是抛异常；
     2. updateGps 每次都检查地图对象是否就绪，坐标照常显示在下方文字里。
   最要命的是不加防护的版本：第一帧 GPS 就抛 TypeError，而它是在 MQTT
   消息回调里抛的，会把整条消息处理链打断——所有传感器数据跟着一起停更，
   现象是"大屏突然不动了"，谁也想不到是地图库没加载。 */
function initMap() {
  if (typeof L === 'undefined') {
    const empty = document.getElementById('mapEmpty');
    if (empty) {
      const b = empty.querySelector('b'), sp = empty.querySelector('span');
      if (b) b.textContent = '地图组件未加载';
      if (sp) sp.textContent = '当前网络访问不到 CDN，地图不可用；GPS 坐标仍会显示在下方';
    }
    console.warn('[map] Leaflet 未加载，地图功能降级为纯坐标显示');
    return;
  }
  gpsMap = L.map('map').setView(GPS_PLACEHOLDER, 15);
  /* 瓦片用高德而不是 OpenStreetMap：
     ① OSM 的瓦片服务器在国内经常加载不出来，大屏上就是一块灰；
     ② 国内公开地图按法规用 GCJ-02，高德瓦片和车载端 Qt 地图是同一套，
        两边看到的位置对得上。 */
  L.tileLayer('https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}', {
    subdomains: ['1', '2', '3', '4'], maxZoom: 18, attribution: '© 高德地图'
  }).addTo(gpsMap);
  gpsMarker = L.marker(GPS_PLACEHOLDER).addTo(gpsMap);
  /* 真实定位到来之前先把实心标记藏起来，免得它被当成车的位置 */
  gpsMap.removeLayer(gpsMarker);
  gpsTrack = L.polyline([], { color: '#3ddc97', weight: 2 }).addTo(gpsMap);

  /* **初始化完就把占位点放上去**，不要等第一条 GPS 消息。
     原来占位点只在 updateGps 里显示，而模块没插的时候 gps_mqtt 一条消息
     都不发——于是"等待 GPS 定位"的遮罩永远撤不掉，地图一直是空的。
     "没有消息"本身就是最常见的情况，得有人管。 */
  showGpsPlaceholder();
  const info = document.getElementById('gpsInfo');
  if (info && !info.textContent.trim())
    info.textContent = '未收到定位数据 · 地图上是 ' + GPS_PLACEHOLDER_NAME + '，演示位置，不是车的实际位置';
}
function updateGps(data) {
  const info = document.getElementById('gpsInfo');
  const empty = document.getElementById('mapEmpty');
  if (!data.fix || data.fix === 0) {
    /* fix=0 说明模块接上了但还没搜到星，跟"压根没接模块"是两回事，
       空状态里说清楚，免得让人去查接线 */
    info.textContent = '未定位（模块在线，正在搜星 fix=0）· 地图上是 '
      + GPS_PLACEHOLDER_NAME + '，演示位置，不是车的实际位置';
    showGpsPlaceholder();
    return;
  }
  const lat = num(data.lat), lon = num(data.lon);
  if (!lat && !lon) {
    /* 0,0 是几内亚湾，几乎一定是无效值而不是真坐标。
       注意这里**也要走占位分支**：网关在丢解时会把坐标清零并保持 fix>0
       的上一个值发出来，光判 fix 会漏掉这种情况，结果是地图停在
       上一个真实点上不动——那比显示占位点更容易骗人。 */
    info.textContent = '坐标无效（0,0）· 地图上是 ' + GPS_PLACEHOLDER_NAME
      + '，演示位置，不是车的实际位置';
    showGpsPlaceholder();
    return;
  }

  /* 真实定位来了：撤掉占位点，把实心标记放回去 */
  hideGpsPlaceholder();
  if (gpsMap && gpsMarker && !gpsMap.hasLayer(gpsMarker)) gpsMarker.addTo(gpsMap);
  info.textContent = '纬度 ' + lat.toFixed(6) + '  经度 ' + lon.toFixed(6) +
    '  卫星 ' + (data.sat || '-') + '  速度 ' + num(data.speed) + ' km/h';

  /* 文字里显示 GNSS 原值（WGS-84），画图用转换后的（GCJ-02）。
     两者故意不一样：前者是给人核对模块读数的，后者是给地图用的。 */
  const pos = wgs84ToGcj02(lat, lon);
  gpsPoints.push(pos);
  if (gpsPoints.length > 200) gpsPoints.shift();

  /* 地图没就绪就到此为止：坐标文字已经更新过了，功能降级但不中断。
     不加这个判断的话会在 MQTT 回调里抛异常，把整条消息处理链打断 */
  if (!gpsMap || !gpsMarker || !gpsTrack) return;

  if (empty) empty.classList.add('hide');
  gpsMarker.setLatLng(pos);
  gpsMap.invalidateSize();   /* 空状态盖着时地图容器尺寸可能没算对 */
  gpsTrack.setLatLngs(gpsPoints);
  gpsMap.setView(pos, gpsMap.getZoom());
}

// ============================================================
// 日志
// ============================================================
function log(msg) {
  const el = document.getElementById('log');
  const line = document.createElement('div');
  line.textContent = '[' + new Date().toLocaleTimeString() + '] ' + msg;
  el.prepend(line);
  while (el.children.length > 30) el.removeChild(el.lastChild);
}

window.addEventListener('resize', () => { drawChart(); drawImuChart(); });
