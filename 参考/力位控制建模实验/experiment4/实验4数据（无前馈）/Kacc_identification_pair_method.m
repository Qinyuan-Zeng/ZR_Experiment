%% ============================================================
%  能量积分法辨识 Kacc
%
%  当前版本：
%  1. 使用 Vcmd 第13列判断加减速段；
%  2. 不再使用 Vffd 第2列；
%  3. 电流使用 Icmd - i_static_mA；
%  4. 能量项和积分项中的速度使用实际速度 Vel；
%  5. 全部使用列号，不使用表头名称。
%
%  能量积分法：
%       0.5 * (v2^2 - v1^2) = Kacc * ∫ i_net * v dt
%
%       Kacc = 0.5*(v2^2-v1^2) / ∫(i_net*v)dt
%
%  单位：
%       v: mm/s
%       i_net: mA
%       Kacc: mm/s^2/mA
%% ============================================================

clear; clc; close all;

%% ===================== 1. 用户参数区 =====================

data_file = ['40_400_1' ...
    '.csv'];   % 修改为你的数据文件

%% ---------- 数据列号 ----------
time_col = 1;      % A列 time(s)
vel_col  = 3;      % C列 Vel，实际速度
icmd_col = 4;      % D列 Icmd，指令电流
z_col    = 8;      % H列 Encoder ACC Feedback，实际位移
vcmd_col = 13;     % M列 Vcmd，指令速度，注意这里不是 Vffd

%% ---------- 单位设置 ----------
z_unit = 'um';       % 第8列如果是 um，保持 um；如果是 mm，改成 mm
v_unit = 'mmps';     % 速度单位，mm/s
i_unit = 'mA';       % 电流单位，mA

%% ---------- 静态补偿表 ----------
use_static_lut = true;

static_lut_file = 'best_down_pchip_lut_anchor_points.csv';
static_pos_col = 1;
static_cur_col = 2;
static_pos_unit = 'um';

%% ---------- 滤波参数 ----------
use_filter = true;

% 用于平滑实际速度、电流、位移
sgolay_order = 3;
sgolay_frame = 51;      % 8000 Hz 数据可适当大一点，比如 51/101

% 用于平滑 Vcmd 再求导
vcmd_smooth_time = 0.020;   % s，5 ms 移动平均

%% ---------- 加减速段识别参数 ----------
% 根据 Vcmd 导数识别加减速段
cmd_acc_th = 80.0;          % mm/s^2，指令加速度阈值

% 合并 mask 中短暂断开的间隙
merge_gap_time = 0.020;     % s，10 ms 内的断裂合并

% 片段最短时间与点数
min_seg_time = 0.050;       % s
min_seg_pts  = 100;          % 8000Hz 下 50点约6.25ms

% 只分析下行段
% 如果下行速度为正，并且你只想辨识下行 Kacc，设 true
only_down_motion = true;

% 实际速度太小的点不参与积分
v_actual_min_abs = 0.2;     % mm/s

% 能量积分分母过小剔除
min_integral_abs = 1e-6;

% 电流方向修正
% 如果结果大量为负，改成 -1
current_sign = 1;

%% ===================== 2. 读取数据 =====================

raw = readmatrix(data_file);
raw = raw(~all(isnan(raw), 2), :);

t_raw    = raw(:, time_col);
v_raw    = raw(:, vel_col);
icmd_raw = raw(:, icmd_col);
z_raw    = raw(:, z_col);
vcmd_raw = raw(:, vcmd_col);

valid = ~isnan(t_raw) & ~isnan(v_raw) & ~isnan(icmd_raw) & ...
        ~isnan(z_raw) & ~isnan(vcmd_raw);

t_raw    = t_raw(valid);
v_raw    = v_raw(valid);
icmd_raw = icmd_raw(valid);
z_raw    = z_raw(valid);
vcmd_raw = vcmd_raw(valid);

% 时间归零
t = t_raw - t_raw(1);

% 去除重复时间点
[t, unique_idx] = unique(t, 'stable');
v_raw    = v_raw(unique_idx);
icmd_raw = icmd_raw(unique_idx);
z_raw    = z_raw(unique_idx);
vcmd_raw = vcmd_raw(unique_idx);

dt_vec = diff(t);
Ts_est = median(dt_vec);
Fs_est = 1 / Ts_est;

fprintf('数据点数: %d\n', length(t));
fprintf('估计采样周期 Ts = %.9f s\n', Ts_est);
fprintf('估计采样频率 Fs = %.3f Hz\n', Fs_est);

%% ===================== 3. 单位转换 =====================

% 位移转 mm
switch lower(z_unit)
    case 'um'
        z_mm = z_raw / 1000.0;
    case 'mm'
        z_mm = z_raw;
    otherwise
        error('z_unit 只能为 um 或 mm');
end

% 实际速度和指令速度转 mm/s
switch lower(v_unit)
    case 'mmps'
        v_mmps    = v_raw;
        vcmd_mmps = vcmd_raw;
    case 'umps'
        v_mmps    = v_raw / 1000.0;
        vcmd_mmps = vcmd_raw / 1000.0;
    otherwise
        error('v_unit 只能为 mmps 或 umps');
end

% 电流转 mA
switch lower(i_unit)
    case 'ma'
        icmd_mA = current_sign * icmd_raw;
    case 'a'
        icmd_mA = current_sign * icmd_raw * 1000.0;
    otherwise
        error('i_unit 只能为 mA 或 A');
end

%% ===================== 4. 信号滤波 =====================

% sgolay frame 合法化
if mod(sgolay_frame, 2) == 0
    sgolay_frame = sgolay_frame + 1;
end

if sgolay_frame >= length(t)
    sgolay_frame = length(t) - 1;
    if mod(sgolay_frame, 2) == 0
        sgolay_frame = sgolay_frame - 1;
    end
end

if use_filter && sgolay_frame > sgolay_order + 2
    z_f    = sgolayfilt(z_mm, sgolay_order, sgolay_frame);
    v_f    = sgolayfilt(v_mmps, sgolay_order, sgolay_frame);
    icmd_f = sgolayfilt(icmd_mA, sgolay_order, sgolay_frame);
else
    z_f    = z_mm;
    v_f    = v_mmps;
    icmd_f = icmd_mA;
end

% Vcmd 用移动平均平滑，避免求导后噪声太大
N_vcmd_smooth = max(3, round(vcmd_smooth_time / Ts_est));
vcmd_f = smoothdata(vcmd_mmps, 'movmean', N_vcmd_smooth);

% 由 Vcmd 计算指令加速度
a_cmd = gradient(vcmd_f, t);

%% ===================== 5. 静态补偿电流 =====================

if use_static_lut
    if exist(static_lut_file, 'file') ~= 2
        warning('未找到静态表文件：%s，i_static 置零。', static_lut_file);
        i_static_mA = zeros(size(t));
    else
        lut_raw = readmatrix(static_lut_file);
        lut_raw = lut_raw(~all(isnan(lut_raw), 2), :);

        z_lut = lut_raw(:, static_pos_col);
        i_lut = lut_raw(:, static_cur_col);

        valid_lut = ~isnan(z_lut) & ~isnan(i_lut);
        z_lut = z_lut(valid_lut);
        i_lut = i_lut(valid_lut);

        switch lower(static_pos_unit)
            case 'um'
                z_lut_um = z_lut;
            case 'mm'
                z_lut_um = z_lut * 1000.0;
            otherwise
                error('static_pos_unit 只能为 um 或 mm');
        end

        [z_lut_um, idx] = sort(z_lut_um);
        i_lut = i_lut(idx);

        z_current_um = z_f * 1000.0;

        % 防止外推
        z_current_um = min(max(z_current_um, min(z_lut_um)), max(z_lut_um));

        i_static_mA = interp1(z_lut_um, i_lut, z_current_um, 'pchip');
    end
else
    i_static_mA = zeros(size(t));
end

%% ===================== 6. 净电流 =====================

% 按你的要求：
% i_net = 指令电流 - 静态补偿电流
i_net_mA = icmd_f - i_static_mA;

%% ===================== 7. 根据 Vcmd 斜率识别加减速段 =====================

% 加速段：Vcmd 斜率大于阈值
acc_mask = a_cmd > cmd_acc_th;

% 减速段：Vcmd 斜率小于负阈值
dec_mask = a_cmd < -cmd_acc_th;

% 只取下行运动时，要求 Vcmd 和实际速度为正
if only_down_motion
    acc_mask = acc_mask & (vcmd_f > 0) & (v_f > v_actual_min_abs);
    dec_mask = dec_mask & (vcmd_f > 0) & (v_f > v_actual_min_abs);
else
    acc_mask = acc_mask & (abs(v_f) > v_actual_min_abs);
    dec_mask = dec_mask & (abs(v_f) > v_actual_min_abs);
end

% 合并短间断
merge_gap_pts = max(1, round(merge_gap_time / Ts_est));

acc_mask = merge_short_gaps(acc_mask, merge_gap_pts);
dec_mask = merge_short_gaps(dec_mask, merge_gap_pts);

% 提取加速和减速片段
segments = [];

segments = [segments; extract_segments_from_mask(acc_mask, t,  1, min_seg_time, min_seg_pts)];
segments = [segments; extract_segments_from_mask(dec_mask, t, -1, min_seg_time, min_seg_pts)];

% 按起始时间排序
if ~isempty(segments)
    [~, sort_idx] = sort(segments(:,1));
    segments = segments(sort_idx, :);
end

fprintf('\n根据 Vcmd 斜率筛选到 %d 个加减速片段。\n', size(segments, 1));

if isempty(segments)
    error(['没有筛选到有效加减速片段，请检查：' newline ...
           '1. Vcmd 是否确实为第13列；' newline ...
           '2. cmd_acc_th 是否过大；' newline ...
           '3. only_down_motion 是否过滤掉了有效片段；' newline ...
           '4. v_actual_min_abs 是否过大。']);
end

%% ===================== 8. 能量积分法计算 Kacc =====================

result = [];

for k = 1:size(segments, 1)

    t1 = segments(k, 1);
    t2 = segments(k, 2);
    seg_type = segments(k, 3);   % 1 加速，-1 减速

    seg = (t >= t1) & (t <= t2);

    if nnz(seg) < min_seg_pts
        continue;
    end

    ts = t(seg);
    vs = v_f(seg);          % 实际速度
    is = i_net_mA(seg);     % Icmd - i_static
    zs = z_f(seg);
    vcs = vcmd_f(seg);
    acs = a_cmd(seg);

    % 能量变化项，使用实际速度
    dE = 0.5 * (vs(end)^2 - vs(1)^2);

    % 输入积分项，使用实际速度
    integral_i_v = trapz(ts, is .* vs);

    if abs(integral_i_v) < min_integral_abs
        continue;
    end

    Kacc_est = dE / integral_i_v;

    % Kacc 应为正
    if Kacc_est <= 0
        continue;
    end

    result = [result; ...
        k, seg_type, t1, t2, t2-t1, ...
        zs(1), zs(end), ...
        vcs(1), vcs(end), ...
        vs(1), vs(end), ...
        mean(acs), ...
        dE, integral_i_v, Kacc_est]; %#ok<AGROW>
end

if isempty(result)
    error(['筛选到了片段，但没有得到有效 Kacc。请检查：' newline ...
           '1. current_sign 是否需要改为 -1；' newline ...
           '2. Icmd - i_static 的符号是否正确；' newline ...
           '3. 减速段是否应单独分析；' newline ...
           '4. 静态补偿表单位是否正确。']);
end

result_table = array2table(result, ...
    'VariableNames', { ...
    'seg_id', 'seg_type', 't_start_s', 't_end_s', 'duration_s', ...
    'z_start_mm', 'z_end_mm', ...
    'vcmd_start_mmps', 'vcmd_end_mmps', ...
    'v_start_mmps', 'v_end_mmps', ...
    'mean_acmd_mmps2', ...
    'delta_half_v2', 'integral_i_v', 'Kacc_mmps2_per_mA'});

%% ===================== 9. 统计结果 =====================

K_list = result_table.Kacc_mmps2_per_mA;

K_median = median(K_list);
K_mean = mean(K_list);
K_std = std(K_list);

weights = abs(result_table.integral_i_v);
K_weighted = sum(K_list .* weights) / sum(weights);

fprintf('\n================ Kacc 辨识结果 ================\n');
fprintf('有效片段数量: %d\n', length(K_list));
fprintf('Kacc 中位数     = %.4f mm/s^2/mA\n', K_median);
fprintf('Kacc 平均值     = %.4f mm/s^2/mA\n', K_mean);
fprintf('Kacc 加权平均值 = %.4f mm/s^2/mA\n', K_weighted);
fprintf('Kacc 标准差     = %.4f mm/s^2/mA\n', K_std);

Kt_N_per_A = 4.12;
M_eff_from_median = Kt_N_per_A / K_median;
M_eff_from_weighted = Kt_N_per_A / K_weighted;

fprintf('\n按 Kt = 4.12 N/A 换算：\n');
fprintf('由 Kacc 中位数估计 M_eff = %.4f kg\n', M_eff_from_median);
fprintf('由 Kacc 加权均值估计 M_eff = %.4f kg\n', M_eff_from_weighted);

disp(result_table);

%% ===================== 10. 绘图检查 =====================

figure('Name', '指令速度判定加减速段');
subplot(4,1,1);
plot(t, vcmd_f, 'LineWidth', 1.2);
grid on;
ylabel('Vcmd / mm/s');
title('指令速度 Vcmd，第13列');

subplot(4,1,2);
plot(t, a_cmd, 'LineWidth', 1.2);
grid on;
ylabel('a\_cmd / mm/s^2');
title('由 Vcmd 得到的指令加速度');

subplot(4,1,3);
plot(t, v_f, 'LineWidth', 1.2);
grid on;
ylabel('Vel / mm/s');
title('实际速度');

subplot(4,1,4);
plot(t, acc_mask, 'g', 'LineWidth', 1.2); hold on;
plot(t, dec_mask, 'r', 'LineWidth', 1.2);
grid on;
ylabel('mask');
xlabel('t / s');
legend('acc mask', 'dec mask');
title('筛选出的加减速片段');

figure('Name', '实际速度与净电流');
yyaxis left;
plot(t, v_f, 'LineWidth', 1.2); hold on;
ylabel('Actual Vel / mm/s');

yyaxis right;
plot(t, i_net_mA, 'LineWidth', 1.2);
ylabel('Icmd - i\_static / mA');

grid on;
xlabel('t / s');
title('实际速度与净电流');

for k = 1:size(segments, 1)
    if segments(k,3) > 0
        xline(segments(k,1), '--g', 'acc start');
        xline(segments(k,2), '--g', 'acc end');
    else
        xline(segments(k,1), '--r', 'dec start');
        xline(segments(k,2), '--r', 'dec end');
    end
end

figure('Name', '电流分量');
plot(t, icmd_f, 'LineWidth', 1.2); hold on;
plot(t, i_static_mA, 'LineWidth', 1.2);
plot(t, i_net_mA, 'LineWidth', 1.2);
grid on;
xlabel('t / s');
ylabel('Current / mA');
legend('Icmd', 'i\_static', 'Icmd - i\_static');
title('参与积分的净电流');

figure('Name', 'Kacc 估计结果');
plot(result_table.seg_id, result_table.Kacc_mmps2_per_mA, 'o-', 'LineWidth', 1.2); hold on;
yline(K_median, '--', 'Median');
yline(K_weighted, '--', 'Weighted');
grid on;
xlabel('Segment ID');
ylabel('Kacc / (mm/s^2/mA)');
title('各加减速片段 Kacc 估计');

%% ===================== 11. 保存结果 =====================

writetable(result_table, 'Kacc_energy_by_Vcmd_slope_results.csv');

summary_table = table(K_median, K_mean, K_weighted, K_std, ...
    M_eff_from_median, M_eff_from_weighted, ...
    'VariableNames', {'Kacc_median', 'Kacc_mean', 'Kacc_weighted', 'Kacc_std', ...
                      'M_eff_from_median_kg', 'M_eff_from_weighted_kg'});

writetable(summary_table, 'Kacc_energy_by_Vcmd_slope_summary.csv');

fprintf('\n结果已保存：\n');
fprintf('  Kacc_energy_by_Vcmd_slope_results.csv\n');
fprintf('  Kacc_energy_by_Vcmd_slope_summary.csv\n');

%% ============================================================
%  本脚本使用的局部函数
%% ============================================================

function mask_out = merge_short_gaps(mask_in, max_gap_pts)
% 合并 mask 中长度不超过 max_gap_pts 的 false 间隙

mask_out = mask_in(:);
N = length(mask_out);

idx_true = find(mask_out);

if isempty(idx_true)
    return;
end

% 找 false gap
i = 1;
while i <= N
    if mask_out(i)
        i = i + 1;
        continue;
    end

    gap_start = i;
    while i <= N && ~mask_out(i)
        i = i + 1;
    end
    gap_end = i - 1;

    gap_len = gap_end - gap_start + 1;

    left_true = gap_start > 1 && mask_out(gap_start - 1);
    right_true = gap_end < N && mask_out(gap_end + 1);

    if left_true && right_true && gap_len <= max_gap_pts
        mask_out(gap_start:gap_end) = true;
    end
end

mask_out = mask_out(:);
end


function segments = extract_segments_from_mask(mask, t, seg_type, min_seg_time, min_seg_pts)
% 从逻辑 mask 中提取连续片段
% 输出 columns:
%   [t_start, t_end, seg_type]

mask = mask(:);
idx = find(mask);
segments = [];

if isempty(idx)
    return;
end

break_pos = [1; find(diff(idx) > 1) + 1; length(idx) + 1];

for k = 1:length(break_pos)-1
    idx_seg = idx(break_pos(k):break_pos(k+1)-1);

    if length(idx_seg) < min_seg_pts
        continue;
    end

    t1 = t(idx_seg(1));
    t2 = t(idx_seg(end));

    if (t2 - t1) < min_seg_time
        continue;
    end

    segments = [segments; t1, t2, seg_type]; %#ok<AGROW>
end
end