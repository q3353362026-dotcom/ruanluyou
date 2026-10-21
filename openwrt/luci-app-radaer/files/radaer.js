'use strict';
'require view';
'require form';
'require uci';
'require rpc';
'require poll';
'require ui';

const getStatus = rpc.declare({ object: 'luci.radaer', method: 'status', expect: { '': {} } });
const getLogs = rpc.declare({ object: 'luci.radaer', method: 'logs', expect: { text: '' } });
const setAction = rpc.declare({ object: 'luci.radaer', method: 'action', params: ['action'], expect: { '': {} } });

return view.extend({
    load: function() {
        return Promise.all([uci.load('radaer'), uci.load('network'), getStatus()]);
    },

    refresh: function() {
        return getStatus().then(L.bind(function(status) {
            this.current = status;
            this.statusText.textContent = status.running ? '运行中' : '未运行';
            this.statusText.style.color = status.running ? 'var(--success-color, #248a46)' : 'var(--error-color, #c44)';
            this.details.textContent = (status.running ? 'PID ' + status.pid + ' · 内存 ' + (status.memoryKB / 1024).toFixed(1) + ' MiB · ' : '') +
                (status.device || status.network || '接口未就绪') + ' · ' + (status.bootEnabled ? '已启用开机启动' : '未启用开机启动') +
                ' · 硬件加速：' + ({enabled:'已开启',disabled:'已关闭',unavailable:'无 MediaTek HNAT 接口'}[status.hardwareAcceleration] || '读取失败');
            this.bootButton.textContent = status.bootEnabled ? '关闭开机启动' : '开启开机启动';
            const valid = /^(\d{1,3}\.){3}\d{1,3}$/.test(status.address || '') && /^\d+$/.test(status.port || '');
            const url = valid ? 'http://' + status.address + ':' + status.port : '';
            for (const item of [[this.consoleLink, ''], [this.radarLink, '/radar'], [this.logsLink, '/#activity']]) {
                if (status.running && url) item[0].setAttribute('href', url + item[1]);
                else item[0].removeAttribute('href');
                item[0].setAttribute('aria-disabled', String(!status.running || !url));
                item[0].style.opacity = status.running && url ? '1' : '.5';
            }
            this.addressText.textContent = url || '等待所选接口获得 IPv4 地址';
        }, this));
    },

    action: function(action) {
        return setAction(action).then(L.bind(function(result) {
            if (result.code !== 0) throw new Error(result.message || '服务操作失败');
            return this.refresh();
        }, this)).catch(function(error) { ui.addNotification(null, E('p', {}, error.message)); });
    },

    render: function(data) {
        const readonly = !L.hasViewPermission() || null;
        this.current = data[2];
        this.statusText = E('strong', {}, '正在读取…');
        this.details = E('p', {}, '');
        this.addressText = E('code', {}, '');
        this.consoleLink = E('a', { 'class': 'btn cbi-button-action', target: '_blank', rel: 'noopener noreferrer' }, '打开采集控制台');
        this.radarLink = E('a', { 'class': 'btn cbi-button-positive', target: '_blank', rel: 'noopener noreferrer' }, '打开雷达');
        this.logsLink = E('a', { 'class': 'btn cbi-button-action', target: '_blank', rel: 'noopener noreferrer' }, '清理采集日志');
        this.bootButton = E('button', { 'class': 'btn cbi-button-action', disabled: readonly,
            click: ui.createHandlerFn(this, function() { return this.action(this.current.bootEnabled ? 'disable' : 'enable'); }) }, '开机启动');
        const actions = ['start', 'stop', 'restart'].map((action, index) => E('button', {
            'class': 'btn ' + (action === 'stop' ? 'cbi-button-negative' : 'cbi-button-action'),
            disabled: readonly, click: ui.createHandlerFn(this, 'action', action)
        }, ['启动服务', '停止服务', '重启服务'][index]));
        const status = E('div', { 'class': 'cbi-section' }, [
            E('h3', {}, ['服务状态：', this.statusText]), this.details,
            E('div', { style: 'display:flex;flex-wrap:wrap;gap:.6em;margin:1em 0' }, actions.concat([this.bootButton])),
            E('div', { style: 'display:flex;flex-wrap:wrap;gap:.6em;margin:1em 0' }, [this.consoleLink, this.radarLink, this.logsLink]),
            E('p', {}, ['局域网访问地址：', this.addressText]),
            E('p', {}, '启动服务时自动关闭 MediaTek 硬件加速，停止服务时自动开启。服务运行后，在采集控制台选择客户机并开始采集。停止或重启服务会结束当前采集和已接入的 TCP 连接。')
        ]);

        const m = new form.Map('radaer', 'Radaer 雷达', '路由器上的 IPv4 流量采集与实时雷达。配置保存并应用后，服务会重新加载。');
        const statusSection = m.section(form.TypedSection, '_status');
        statusSection.render = function() { return status; };
        const s = m.section(form.NamedSection, 'main', 'radaer', '服务设置');
        s.addremove = false;
        let o = s.option(form.Flag, 'enabled', '启用服务', '关闭后应用配置将停止服务；开机启动由上方按钮独立控制。');
        o.rmempty = false; o.default = '1';
        o = s.option(form.ListValue, 'network', '监听网络', '选择客户机连接的 LAN 网络。网页仅监听该网络的 IPv4 地址。');
        uci.sections('network', 'interface', function(section) {
            if (section['.name'] !== 'loopback') o.value(section['.name']);
        });
        o.rmempty = false; o.default = 'lan';
        o = s.option(form.Value, 'port', '网页端口');
        o.datatype = 'port'; o.rmempty = false; o.default = '8765';
        o = s.option(form.Value, 'output_directory', '采集保存目录', '使用 Extroot 时保存在 U 盘。可在采集控制台清理历史日志，正在写入的日志和 bank 文件会保留。');
        o.rmempty = false; o.default = '/opt/radaer/captures';
        o.validate = function(section, value) {
            return value && value[0] === '/' && !/[\x00-\x1f]/.test(value) ? true : '请输入以 / 开头的绝对目录路径';
        };

        const logText = E('pre', { style: 'max-height:22em;overflow:auto;white-space:pre-wrap;word-break:break-word' }, '点击“刷新日志”查看最近 60 条服务日志。');
        const logs = E('div', { 'class': 'cbi-section' }, [E('h3', {}, '运行日志'),
            E('button', { 'class': 'btn cbi-button-action', click: ui.createHandlerFn(this, function() {
                return getLogs().then(function(text) { logText.textContent = text || '暂无日志'; });
            }) }, '刷新日志'), logText]);
        return m.render().then(L.bind(function(formNode) {
            const container = E('div', {}, [formNode, logs]);
            poll.add(L.bind(this.refresh, this), 5);
            return this.refresh().then(function() { return container; });
        }, this));
    }
});
