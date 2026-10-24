"use strict";
const $ = (id) => document.getElementById(id);
const model = {
  token: "",
  adapters: [],
  scan: { running: false, devices: [] },
  capture: { running: false },
  selected: "",
  polling: false,
  connected: false,
  logs: "",
};
const phases = {
  idle: "未启动",
  starting: "启动中",
  running: "采集中",
  stopping: "停止中",
  completed: "已结束",
  failed: "采集异常",
};
const eventNames = {
  session_started: "采集会话开始",
  shared_capture_started: "共享网络采集已接入",
  session_stopped: "采集会话结束",
  bank_candidate: "发现 bank 候选",
  bank_bound: "UDP bank 绑定成功",
  bank_unbound: "bank 绑定解除",
  capture_gap: "记录观察缺口",
  tcp_connected: "TCP 连接已接入",
  tcp_error: "TCP 连接异常",
  tcp_rejected: "TCP 连接未接入",
  dh_established: "DH 会话已建立",
  battle_welcome: "确认战斗流",
  battle_state: "战斗流状态变化",
};
function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}
function message(text, good = false) {
  const box = $("globalMessage");
  box.textContent = text;
  box.className = "message" + (good ? " success" : "");
}
function pill(element, text, kind) {
  element.textContent = text;
  element.className = "pill " + kind;
}
function when(ms) {
  return ms ? new Date(ms).toLocaleTimeString("zh-CN", { hour12: false }) : "—";
}
function selectedAdapter() {
  return model.adapters.find((a) => a.key === $("adapter").value);
}
async function api(path, body, signal) {
  const options = { cache: "no-store", headers: {}, signal };
  if (body !== undefined) {
    options.method = "POST";
    options.headers = {
      "Content-Type": "application/json",
      "X-Radaer-Token": model.token,
    };
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  let data;
  try {
    data = await response.json();
  } catch {
    throw new Error("服务返回了无法读取的响应");
  }
  if (!response.ok)
    throw new Error(data.error || "请求失败：" + response.status);
  return data;
}
function applyNetwork(network) {
  const previous = $("adapter").value;
  model.adapters = [];
  for (const adapter of network.adapters || []) {
    if (!adapter.up) continue;
    for (const ip of adapter.ipv4 || []) {
      if (ip.address.startsWith("127.") || ip.address.startsWith("169.254."))
        continue;
      model.adapters.push({
        ...adapter,
        hostIp: ip.address,
        prefix: ip.prefixLength,
        key: adapter.ifIndex + "|" + ip.address,
      });
    }
  }
  const score = (a) =>
    (a.hostIp === location.hostname ? 100 : 0) +
    (/virtual|vmware|hyper-v|loopback/i.test(a.description) ? 0 : 10) +
    ((a.gateways || []).length ? 5 : 0);
  model.adapters.sort((a, b) => score(b) - score(a));
  $("adapter").replaceChildren();
  for (const a of model.adapters) {
    const option = node(
      "option",
      "",
      a.name + " · " + a.hostIp + "/" + a.prefix
    );
    option.value = a.key;
    $("adapter").append(option);
  }
  if (model.adapters.some((a) => a.key === previous))
    $("adapter").value = previous;
  $("privilege").textContent = network.elevated
    ? "管理员模式"
    : "普通权限 · 扫描可用";
  if (network.platform === "linux") {
    $("privilege").textContent = network.elevated
      ? "CaiB 采集服务运行中"
      : "普通权限 · 无法采集";
    document.querySelector(".sidebar-note p").textContent =
      "客户机 → 路由器 → 互联网";
    document.querySelector(".heading p").textContent =
      "选择 LAN 中的客户机，采集经过路由器的 IPv4 流量。";
    $("emptyDevices").querySelector("p").textContent =
      "让客户机连接路由器 LAN 或 Wi-Fi；也可以直接输入客户机 IP。";
    document.querySelector(".capture-note").textContent =
      "客户机默认网关应指向这台路由器。设备列表来自邻居缓存，不代表当前在线。";
  }
  if (!model.adapters.length) {
    $("adapter").append(node("option", "", "未发现可用 IPv4 网卡"));
    message("未发现启用的 IPv4 网卡。请连接网络或打开主机热点。");
  }
  updateRangeHint();
}
function updateRangeHint() {
  const a = selectedAdapter();
  if (a)
    $("scanRange").textContent =
      "接口 #" + a.ifIndex + " · 本机 " + a.hostIp + "/" + a.prefix;
}
async function loadBanks(ids) {
  const previous = $("bankId").value;
  const values = ids || (await api("/api/banks"));
  $("bankId").replaceChildren();
  const empty = node("option", "", "不加载，等待本次会话获取");
  empty.value = "";
  $("bankId").append(empty);
  for (const id of values) {
    const option = node("option", "", id.slice(0, 18) + "…");
    option.value = id;
    $("bankId").append(option);
  }
  if (values.includes(previous)) $("bankId").value = previous;
}
function chooseDevice(ip) {
  if (model.capture.running) return;
  model.selected = ip;
  $("clientIp").value = ip;
  if (!$("deviceName").value) $("deviceName").value = ip;
  $("selectionHint").textContent =
    "已选择客户机 " + ip + "，请确认它通过此网卡共享上网。";
  $("preflightResult").classList.add("hidden");
  renderDevices();
}
function renderDevices() {
  const query = $("search").value.trim().toLowerCase();
  const devices = model.scan.devices || [];
  const clients = devices.filter((d) => !d.self);
  $("deviceCount").textContent = clients.length;
  $("onlineCount").textContent =
    clients.filter((d) => d.online).length +
    " 台在线 · " +
    clients.filter((d) => !d.online).length +
    " 台待确认";
  const filtered = devices.filter((d) =>
    (d.ip + " " + (d.mac || "") + " " + (d.name || ""))
      .toLowerCase()
      .includes(query)
  );
  $("deviceRows").replaceChildren();
  for (const device of filtered) {
    const row = node("tr", model.selected === device.ip ? "selected" : "");
    const first = node("td");
    const cell = node("div", "device-cell");
    cell.append(
      node(
        "span",
        "device-icon",
        device.self ? "▣" : device.gateway ? "◇" : "▤"
      )
    );
    const identity = node("div");
    identity.append(node("div", "device-ip", device.ip));
    identity.append(
      node(
        "div",
        "device-label",
        device.self
          ? "采集主机"
          : device.gateway
          ? "网关设备"
          : device.name || "未识别设备"
      )
    );
    cell.append(identity);
    first.append(cell);
    const mac = node("td", "mac", device.mac || "—");
    const status = node("td");
    const badge = node("span");
    pill(
      badge,
      device.self ? "本机" : device.online ? "在线" : "待确认",
      device.self ? "neutral" : device.online ? "good" : "warning"
    );
    status.append(badge);
    if (device.rttMs !== undefined && device.rttMs !== null)
      status.append(node("div", "device-label", device.rttMs + " ms · ICMP"));
    else if (!device.self)
      status.append(
        node(
          "div",
          "device-label",
          device.evidence === "neighbor_reachable" ? "邻居可达" : "邻居缓存"
        )
      );
    const action = node("td");
    const button = node(
      "button",
      "select-client",
      device.self
        ? "当前主机"
        : model.selected === device.ip
        ? "已选择 ✓"
        : "选为客户机"
    );
    button.type = "button";
    button.disabled = !!device.self || model.capture.running;
    button.addEventListener("click", () => chooseDevice(device.ip));
    action.append(button);
    row.append(first, mac, status, action);
    $("deviceRows").append(row);
  }
  $("emptyDevices").classList.toggle("hidden", filtered.length > 0);
  if (!filtered.length) {
    $("emptyDevices").querySelector("h3").textContent = query
      ? "没有匹配的设备"
      : model.scan.running
      ? "正在发现设备"
      : "尚未发现客户机";
  }
}
function renderScan() {
  const scan = model.scan;
  const a = selectedAdapter();
  const matches = a && scan.ifIndex === a.ifIndex && scan.hostIp === a.hostIp;
  $("scanButton").disabled = scan.running || !a;
  $("stopScan").classList.toggle("hidden", !scan.running);
  pill(
    $("scanBadge"),
    scan.running
      ? "扫描中"
      : scan.error
      ? "扫描异常"
      : scan.finishedAtMs
      ? "已完成"
      : "待扫描",
    scan.running ? "busy" : scan.error ? "error" : "neutral"
  );
  $("scanProgress").max = Math.max(scan.total || 0, 1);
  $("scanProgress").value = scan.completed || 0;
  if (matches && scan.rangeStart)
    $("scanRange").textContent =
      scan.rangeStart +
      " — " +
      scan.rangeEnd +
      (scan.limited ? " · 大网段仅扫描本机所在 /24" : "");
  $("scanSummary").textContent = scan.running
    ? "已探测 " + scan.completed + " / " + scan.total + " 个地址"
    : scan.finishedAtMs
    ? "最近完成 " +
      when(scan.finishedAtMs) +
      (scan.cancelled ? " · 已停止" : "")
    : "等待首次扫描";
  if (scan.error) $("scanSummary").textContent = scan.error;
  else if (scan.warning) $("scanSummary").textContent = scan.warning;
  renderDevices();
}
function renderCapture() {
  const capture = model.capture;
  const counts = capture.counts || {};
  renderRates(capture);
  const fragments = capture.reassembly || capture.stats;
  $("reassemblyStatus").textContent =
    fragments?.ipv4FragmentsReceived !== undefined
      ? [
          ["分片", "ipv4FragmentsReceived"],
          ["重组成功", "ipv4DatagramsReassembled"],
          ["重复", "ipv4DuplicateFragments"],
          ["等待", "ipv4ReassemblyPending"],
          ["超时", "ipv4ReassemblyTimeouts"],
          ["拒绝", "ipv4ReassemblyRejected"],
        ]
          .map(
            ([label, key]) =>
              label + " " + (fragments[key] || 0).toLocaleString("zh-CN")
          )
          .join(" · ")
      : "等待采集统计";
  $("tcpListener").classList.toggle("hidden", !capture.listener);
  $("tcpListener").textContent = capture.listener
    ? "本次 TCP 监听：" + capture.listener.listen
    : "";
  $("rawCount").textContent = (counts.udp_raw || 0).toLocaleString();
  $("plainCount").textContent = (counts.udp_plaintext || 0).toLocaleString();
  $("capturePhase").textContent = phases[capture.phase] || "未启动";
  $("captureTime").textContent = capture.running
    ? "客户机 " +
      capture.clientIp +
      " · 已运行 " +
      Math.floor((Date.now() - capture.startedAtMs) / 1000) +
      " 秒 · " +
      (capture.seconds === 0 ? "不限时" : "限时 " + capture.seconds + " 秒")
    : capture.finishedAtMs
    ? "结束于 " + when(capture.finishedAtMs)
    : "选择客户机后开始";
  $("captureFields").disabled = capture.running;
  $("adapter").disabled = capture.running;
  $("refreshNetwork").disabled = capture.running;
  $("startCapture").classList.toggle("hidden", capture.running);
  $("stopCapture").classList.toggle("hidden", !capture.running);
  $("stopCapture").disabled = capture.phase === "stopping";
  $("preflightButton").disabled = capture.running;
  $("reportButton").disabled = capture.running || !capture.output;
  if (capture.output) $("outputDirectory").textContent = capture.output;
  else if (capture.saveLogs === false)
    $("outputDirectory").textContent = "本次未保存日志 · 无回放记录";
  renderPlayers(capture);
  const signature = JSON.stringify([capture.events, capture.error]);
  if (signature !== model.logs) {
    model.logs = signature;
    const box = $("eventLog");
    box.replaceChildren();
    for (const event of [...(capture.events || [])].reverse()) {
      const line = node("div", "event");
      line.append(node("time", "", when(event.timestampMs)));
      const body = node("div");
      body.append(node("strong", "", eventNames[event.type] || event.type));
      const details = Object.entries(event.data || {})
        .map(([key, value]) => key + ": " + String(value))
        .join(" · ");
      if (details) body.append(node("p", "", details));
      line.append(body);
      box.append(line);
    }
    if (capture.error) {
      const error = node("p", "message", capture.error);
      box.prepend(error);
    }
    if (!box.children.length)
      box.append(node("p", "quiet", "采集启动后，这里会显示本次会话的事件。"));
  }
}
function ageText(ms) {
  return Number.isFinite(ms)
    ? ms < 1000
      ? Math.round(ms) + " ms"
      : (ms / 1000).toFixed(1) + " 秒"
    : "—";
}
function renderRates(capture) {
  const rates = capture?.rates;
  const ready = !!rates;
  for (const [id, key] of [
    ["decryptRate", "udpDecryptPacketsPerSecond"],
    ["coordinateRate", "coordinatePacketsPerSecond"],
    ["coordinateSamplesRate", "coordinateSamplesPerSecond"],
  ])
    $(id).textContent =
      ready && Number.isFinite(rates[key])
        ? rates[key].toLocaleString("zh-CN")
        : "—";
  $("ratesStatus").textContent = ready
    ? "最近 1 秒 · 按本机收到的数据统计"
    : "服务不可达，速率暂停";
  $("coordinateFreshness").textContent =
    ready && Number.isFinite(rates.lastCoordinateAgeMs)
      ? "距上次坐标更新 " + ageText(rates.lastCoordinateAgeMs)
      : "等待坐标更新";
}
function renderPlayers(capture) {
  const counts = capture.counts || {};
  const battle = capture.battle || { status: "waiting" };
  const labels = {
    waiting: "等待战斗流",
    recovery_required: "采集丢包，等待重新进入对局",
    live: "战斗流活跃",
    idle: "战斗流空闲",
    stopped: "采集已停止",
    error: "采集异常",
  };
  pill(
    $("battleStatus"),
    labels[battle.status] || "等待战斗流",
    battle.status === "live"
      ? "good"
      : ["idle", "recovery_required"].includes(battle.status)
      ? "warning"
      : battle.status === "error"
      ? "error"
      : "neutral"
  );
  $("battleDetail").textContent = battle.flow
    ? (battle.map || "未知地图") +
      " · " +
      battle.flow +
      " · 会话 " +
      battle.epoch
    : "确认战斗连接后自动显示人物";
  const emptyText = {
    waiting: [
      "等待战斗连接",
      "等待服务端确认进入地图；开始采集后重新进入对局。",
    ],
    recovery_required: [
      "采集丢包，请重新进入对局",
      "采集仍在运行；重新确认战斗连接后自动恢复，旧人物身份已清除。",
    ],
    live: ["等待人物数据", "当前战斗流已确认，等待可解析的人物更新。"],
    idle: [
      "暂未收到战斗数据",
      "已超过 15 秒没有收到解密后的 UDP 数据；恢复后自动显示。",
    ],
    stopped: [
      "采集已停止",
      capture.saveLogs === false
        ? "本次未保存日志，无回放记录。"
        : "历史人物数据已保存在采集日志中。",
    ],
    error: ["采集异常", "请查看事件记录中的错误信息。"],
  }[battle.status] || ["等待人物数据", ""];
  $("emptyPlayers").querySelector("h3").textContent = emptyText[0];
  $("emptyPlayers").querySelector("p").textContent = emptyText[1];
  $("playerSummary").textContent =
    (counts.player_sample || 0).toLocaleString() +
    " 条坐标样本 · " +
    (counts.player_decode_error || 0).toLocaleString() +
    " 条解析错误 · " +
    (counts.player_decode_warning || 0).toLocaleString() +
    " 条部分解析提示";
  const rows = capture.players || [];
  $("playerRows").replaceChildren();
  for (const p of [...rows].reverse()) {
    const row = node("tr");
    const identity = node("td");
    identity.append(node("div", "device-ip", p.name || "未关联身份"));
    identity.append(
      node(
        "div",
        "device-label",
        "通道 " +
          p.channel +
          " / 槽位 " +
          p.slot +
          " · 队伍 " +
          (p.teamId ?? "—")
      )
    );
    identity.title = p.flow + " · " + p.identitySource;
    const position = node(
      "td",
      "mac",
      [p.x, p.y, p.z]
        .map((n) => (Number.isFinite(n) ? n.toFixed(1) : "—"))
        .join(", ")
    );
    const angle = node(
      "td",
      "mac",
      Number.isFinite(p.yaw) ? p.yaw.toFixed(1) + "°" : "—"
    );
    const speed = node("td", "update-speed");
    speed.append(
      node(
        "div",
        "mac",
        (p.updatesPerSecond || 0).toLocaleString("zh-CN") + " Hz"
      )
    );
    speed.append(
      node(
        "div",
        "device-label",
        Number.isFinite(p.lastUpdateAgeMs)
          ? ageText(p.lastUpdateAgeMs) + " 前更新"
          : "本次切流后暂无更新"
      )
    );
    speed.title =
      "最近 1 秒此槽位的位置样本数；相邻源包时间间隔：" +
      ageText(p.sourceIntervalMs) +
      "（非网络延迟）";
    const quality = node("td");
    const badge = node("span");
    const stale = Date.now() - p.timestampMs > 2000;
    const labels = {
      plausible: "合理性通过",
      warming_up: "积累样本",
      rejected: "未通过",
    };
    pill(
      badge,
      stale ? "历史样本" : labels[p.status] || "待验证",
      stale
        ? "neutral"
        : p.status === "plausible"
        ? "good"
        : p.status === "rejected"
        ? "error"
        : "warning"
    );
    quality.append(badge, node("div", "device-label", when(p.timestampMs)));
    if (p.decodeCompleteness === "known_prefix")
      quality.append(node("div", "device-label", "部分解析：已读取坐标"));
    const reasons = {
      decoder_error_during_sample: "本通道或包结构错误影响连续性",
      partial_movement_block: "后续字段未解析，仅使用此前已读坐标",
      UE_handshake_unconfirmed: "尚未确认战斗握手",
      out_of_order: "样本时间或序号倒退",
      sample_gap: "样本间隔过长，重新积累",
      duplicate_pose: "重复坐标样本",
      future_timestamp: "样本时间超前",
      stale: "样本已过期",
      outside_configured_world: "坐标超出配置范围",
      speed_exceeds_configured_limit: "速度超出配置范围",
      nonfinite_or_invalid_timestamp: "坐标或时间无效",
    };
    quality.title = (p.flags || [])
      .map((f) => reasons[f] || f)
      .concat(p.decodeStopReason || [])
      .join(" · ");
    row.append(identity, position, angle, speed, quality);
    $("playerRows").append(row);
  }
  $("emptyPlayers").classList.toggle("hidden", rows.length > 0);
}
function captureSettings() {
  const a = selectedAdapter();
  if (!a) throw new Error("请先选择可用的网卡");
  const redirect = $("redirectTcp").checked;
  return {
    ifIndex: a.ifIndex,
    hostIp: a.hostIp,
    clientIp: $("clientIp").value.trim(),
    device: $("deviceName").value.trim() || $("clientIp").value.trim(),
    seconds: Number($("seconds").value),
    udpPort: Number($("udpPort").value),
    redirectTcp: redirect,
    tcpPort: redirect ? 65010 : 0,
    maxLogBytes: Number($("maxLogBytes").value),
    saveLogs: $("saveLogs").checked,
    saveBanks: $("saveLogs").checked && $("saveBanks").checked,
    bankIds: $("bankId").value ? [$("bankId").value] : [],
  };
}
async function scan() {
  const a = selectedAdapter();
  if (!a || model.scan.running) return;
  $("scanButton").disabled = true;
  try {
    model.scan = await api("/api/scan/start", {
      ifIndex: a.ifIndex,
      hostIp: a.hostIp,
      active: true,
    });
    renderScan();
  } catch (error) {
    message(error.message);
    $("scanButton").disabled = false;
  }
}
async function poll() {
  if (model.polling || !model.token) return;
  model.polling = true;
  try {
    const previous = model.capture.running;
    const data = await api("/api/state");
    model.scan = data.scan;
    model.capture = data.capture;
    model.connected = true;
    pill($("connection"), "● 本机已连接", "good");
    renderCapture();
    renderScan();
    if (previous && !model.capture.running) await loadBanks();
    $("lastUpdated").textContent = "更新于 " + when(Date.now());
  } catch (error) {
    model.connected = false;
    pill($("connection"), "连接已断开", "error");
    $("lastUpdated").textContent = "服务不可达，请检查启动窗口";
    renderRates(null);
  } finally {
    model.polling = false;
  }
}
async function action(fn) {
  try {
    await fn();
  } catch (error) {
    message(error.message);
  }
}
$("scanButton").addEventListener("click", scan);
$("stopScan").addEventListener("click", () =>
  action(async () => {
    await api("/api/scan/stop", {});
    await poll();
  })
);
$("search").addEventListener("input", renderDevices);
$("adapter").addEventListener("change", () =>
  action(async () => {
    if (model.scan.running) {
      await api("/api/scan/stop", {});
      for (let i = 0; i < 30; i++) {
        await new Promise((resolve) => setTimeout(resolve, 200));
        const current = await api("/api/state");
        model.scan = current.scan;
        if (!model.scan.running) break;
      }
    }
    model.selected = "";
    $("clientIp").value = "";
    $("deviceName").value = "";
    $("selectionHint").textContent = "网卡已切换，请重新选择客户机。";
    model.scan.devices = [];
    updateRangeHint();
    renderDevices();
    if ($("autoScan").checked) await scan();
  })
);
$("refreshNetwork").addEventListener("click", () =>
  action(async () => {
    applyNetwork(await api("/api/network"));
    message("网卡列表已刷新。", true);
  })
);
try {
  $("saveLogs").checked = localStorage.getItem("radaer.saveLogs") !== "false";
} catch {}
function updateLogging() {
  $("saveBanks").disabled = !$("saveLogs").checked;
}
updateLogging();
$("saveLogs").addEventListener("change", () => {
  updateLogging();
  try {
    localStorage.setItem("radaer.saveLogs", String($("saveLogs").checked));
  } catch {}
});
$("maxLogBytes").addEventListener("change", () => {
  const value = Number($("maxLogBytes").value);
  if (Number.isInteger(value) && value >= 1024 && value <= 2147483648) {
    try {
      localStorage.setItem("radaer.maxLogBytes", String(value));
    } catch {}
  }
});
$("redirectTcp").addEventListener("change", () => {
  $("tcpFields").classList.toggle("hidden", !$("redirectTcp").checked);
});
$("clientIp").addEventListener("input", () => {
  model.selected = $("clientIp").value.trim();
  $("preflightResult").classList.add("hidden");
});
$("preflightButton").addEventListener("click", () =>
  action(async () => {
    if (!$("captureForm").reportValidity()) return;
    $("preflightButton").disabled = true;
    try {
      const result = await api("/api/capture/preflight", captureSettings());
      const box = $("preflightResult");
      box.textContent =
        "配置与过滤器检查通过。" +
        (result.elevated ? "" : "请以管理员身份重启 WebUI 后采集。") +
        " 仍需确认客户机的默认网关和网络连通性。";
      box.classList.remove("hidden");
    } finally {
      $("preflightButton").disabled = false;
    }
  })
);
$("captureForm").addEventListener("submit", (event) => {
  event.preventDefault();
  action(async () => {
    $("startCapture").disabled = true;
    try {
      model.capture = await api("/api/capture/start", captureSettings());
      $("reportDetails").classList.add("hidden");
      renderCapture();
      renderDevices();
    } finally {
      $("startCapture").disabled = false;
    }
  });
});
$("stopCapture").addEventListener("click", () =>
  action(async () => {
    model.capture = await api("/api/capture/stop", {});
    renderCapture();
  })
);
$("reportButton").addEventListener("click", () =>
  action(async () => {
    const report = await api("/api/capture/report", {});
    $("reportOutput").textContent = JSON.stringify(report, null, 2);
    $("reportDetails").classList.remove("hidden");
    $("reportDetails").open = true;
  })
);
$("clearLogsButton").addEventListener("click", () =>
  $("clearLogsDialog").showModal()
);
$("cancelClearLogs").addEventListener("click", () =>
  $("clearLogsDialog").close()
);
$("confirmClearLogs").addEventListener("click", () =>
  action(async () => {
    const button = $("confirmClearLogs");
    button.disabled = true;
    try {
      const result = await api("/api/logs/clear", {});
      const freed = (result.bytes / 1048576).toFixed(1);
      const failed = result.failed?.length || 0;
      message(
        `已清理 ${result.removed} 条历史采集日志，释放 ${freed} MiB；${result.preservedActive} 条正在写入的日志已保留。` +
          (failed ? `另有 ${failed} 条删除失败，请检查目录权限。` : ""),
        !failed
      );
      $("clearLogsDialog").close();
      if (!result.preservedActive && !failed)
        $("outputDirectory").textContent = model.outputDirectory;
      $("reportDetails").classList.add("hidden");
      await poll();
    } finally {
      button.disabled = false;
    }
  })
);
$("autoScan").addEventListener("change", () => {
  if ($("autoScan").checked) scan();
});
async function init() {
  try {
    const data = await api("/api/bootstrap");
    model.token = data.token;
    model.outputDirectory = data.outputDirectory;
    applyNetwork(data.network);
    $("outputDirectory").textContent = data.outputDirectory;
    await loadBanks(data.bankIds);
    await poll();
    try {
      const saved = Number(localStorage.getItem("radaer.maxLogBytes"));
      if (Number.isInteger(saved) && saved >= 1024 && saved <= 2147483648)
        $("maxLogBytes").value = String(saved);
    } catch {}
    const lan = $("lanAccess");
    lan.replaceChildren();
    if (data.lanEnabled) {
      // 雷达 UI 跑在游戏电脑上的 CaiB雷达.exe（默认 8766），不在路由器上
      lan.append(node("div", "", "雷达界面在游戏电脑上运行，默认地址："));
      const row = node("div"),
        link = node("a", "", "http://127.0.0.1:8766");
      link.href = "http://127.0.0.1:8766";
      link.target = "_blank";
      link.rel = "noopener";
      row.append(link);
      lan.append(row);
      lan.append(
        node(
          "div",
          "",
          "（在游戏电脑上打开；若改了 --port，把 8766 换成实际端口）"
        )
      );
    } else lan.textContent = "仅本机访问；通过默认启动脚本可开启局域网访问。";
    const current = model.capture.running
      ? model.capture.configuration
      : model.scan;
    if (current && current.ifIndex) {
      const key = current.ifIndex + "|" + current.hostIp;
      if (model.adapters.some((a) => a.key === key)) $("adapter").value = key;
    }
    if (model.capture.running && current) {
      $("clientIp").value = current.clientIp || "";
      $("deviceName").value = current.device || "";
      model.selected = current.clientIp || "";
      for (const field of ["seconds", "udpPort", "maxLogBytes"])
        if (current[field] !== undefined) $(field).value = current[field];
      $("saveLogs").checked = current.saveLogs !== false;
      updateLogging();
      $("redirectTcp").checked = !!current.redirectTcp;
      $("saveBanks").checked = !!current.saveBanks;
      $("tcpFields").classList.toggle("hidden", !current.redirectTcp);
      $("selectionHint").textContent =
        "正在采集 " + current.clientIp + "；刷新页面不会中断采集。";
    }
    renderScan();
    renderCapture();
    if ($("autoScan").checked && selectedAdapter() && !model.scan.running)
      await scan();
  } catch (error) {
    pill($("connection"), "连接失败", "error");
    message(error.message);
  }
}
init();
setInterval(poll, 500);
setInterval(() => {
  if (
    document.visibilityState === "visible" &&
    $("autoScan").checked &&
    model.connected &&
    !model.scan.running
  )
    scan();
}, 60000);
