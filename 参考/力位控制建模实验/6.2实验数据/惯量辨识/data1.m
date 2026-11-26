%% ============================================================
%  固定速度上限、多加速度实验辨识 Kacc
%
%  实验思想：
%      固定 Vmax，只改变加速度 Acc；
%      在相同实际速度区间内：
%
%          i_res = Icmd - i_static(z)
%          i_res = b(v) + ca * a
%
%      其中：
%          ca = 1 / Kacc
%          Kacc = 1 / ca
%
%  默认数据列号：
%      第1列  time(s)
%      第3列  Vel，实际速度
%      第4列  Icmd，指令电流
%      第8列  Encoder ACC Feedback，实际位移
%      第13列 Vcmd，指令速度
%
%  单位：
%      z: mm
%      v: mm/s
%      a: mm/s^2
%      current: mA
%      Kacc: mm/s^2/mA
%% ============================================================

clear; clc; close all;

%% ===================== 1. 用户参数区 =====================

% ---------- 数据文件 ----------
% 方法1：手动指定
data_files = {
    '30_100_1.csv'
    '30_100_2.csv'
    '30_100_3.csv'
    '30_200_1.csv'
    '30_200_2.csv'
    '30_200_3.csv'
    '30_400_1.csv'
    '30_400_2.csv'
    '30_400_3.csv'
    '30_600_1.csv'
    '30_600_2.csv'
    '30_600_3.csv'
};

% 方法2：自动读取当前文件夹下所有 csv
% d = dir('*.csv');
% data_files = {d.name}';
% data_files = data_files(~contains(data_files, 'Kacc_'));
% data_files = data_files(~contains(data_files, 'pchip'));
% data_files = data_files(~contains(data_files, 'lut'));

% ---------- 数据列号 ----------
time_col = 1;      % time(s)
vel_col  = 3;      % Vel，实际速度
icmd_col = 4;      % Icmd，指令电流
z_col    = 8;      % Encoder ACC Feedback，实际位移
vcmd_col = 2;     % Vffd，指令速度

% 如果你想改用反馈电流 Ifb，把 icmd_col 改成对应列号即可
% 例如：
% icmd_col = 14;

% ---------- 单位 ----------
z_unit = 'um';       % 第8列若为 um，保持 um；若为 mm，改成 mm
v_unit = 'mmps';     % 速度单位，mm/s
i_unit = 'mA';       % 电流单位，mA

% ---------- 静态补偿表 ----------
use_static_lut = true;

static_lut_file = 'best_down_pchip_lut_anchor_points.csv';
static_pos_col = 1;
static_cur_col = 2;
static_pos_unit = 'um';

% 如果当前实验位移零点和静态表零点不一致，在这里加偏置
% 例如当前实验 z=0 对应静态表 z=1000um，则设 z_offset_um = 1000
z_offset_um = 0.0;

% ---------- 滤波 ----------
use_filter = true;

sgolay_order = 3;
sgolay_frame = 51;          % 对 z/v/current 平滑，8000Hz 数据可用 51 或 101

vcmd_smooth_time = 0.020;   % s，Vcmd 平滑后用于分段
acc_smooth_time  = 0.010;   % s，实际加速度平滑

% ---------- 加速段识别 ----------
cmd_acc_th = 80.0;          % mm/s^2，用 Vcmd 导数识别加速段
merge_gap_time = 0.020;     % s，合并加速 mask 中短断裂
min_seg_time = 0.050;       % s，最短加速段时间
min_seg_pts  = 100;         % 最少点数
min_vcmd_delta = 5.0;       % mm/s，加速段内 Vcmd 至少变化量
trim_time = 0.005;          % s，裁剪加速段边界

% 只使用下行加速段
only_down_motion = true;

% ---------- 速度分箱 ----------
v_bin_min = 5.0;            % mm/s
v_bin_max = 30.0;           % mm/s
v_bin_width = 5.0;          % mm/s

min_bin_pts_per_file = 30;  % 单个文件在某速度箱内至少点数
min_fit_points = 4;         % 每个速度箱至少来自多少个文件/加速度点
min_acc_span = 100.0;       % mm/s^2，该速度箱内加速度范围至少这么大

% ---------- 结果筛选 ----------
Kacc_min = 2.0;             % 物理合理下限
Kacc_max = 80.0;            % 物理合理上限

% 电流方向
% 如果得到大量负 Kacc，把 current_sign 改为 -1
current_sign = 1;

% ---------- 绘图 ----------
plot_each_file = false;
plot_fit_each_bin = true;

%% ===================== 2. 初始化汇总表 =====================

all_bin_points = table();
all_file_summary = table();

%% ===================== 3. 逐文件处理 =====================

for file_id = 1:numel(data_files)

    data_file = data_files{file_id};

    if exist(data_file, 'file') ~= 2
        warning('文件不存在，跳过：%s', data_file);
        continue;
    end

    fprintf('\n====================================================\n');
    fprintf('正在处理文件：%s\n', data_file);
    fprintf('====================================================\n');

    %% ---------- 读取数据 ----------
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

    %% ---------- 单位转换 ----------
    switch lower(z_unit)
        case 'um'
            z_mm = z_raw / 1000.0;
        case 'mm'
            z_mm = z_raw;
        otherwise
            error('z_unit 只能为 um 或 mm');
    end

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

    switch lower(i_unit)
        case 'ma'
            icmd_mA = current_sign * icmd_raw;
        case 'a'
            icmd_mA = current_sign * icmd_raw * 1000.0;
        otherwise
            error('i_unit 只能为 mA 或 A');
    end

    %% ---------- 滤波 ----------
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

    %% ---------- Vcmd 平滑并求指令加速度 ----------
    N_vcmd_smooth = max(5, round(vcmd_smooth_time / Ts_est));
    if mod(N_vcmd_smooth, 2) == 0
        N_vcmd_smooth = N_vcmd_smooth + 1;
    end

    vcmd_tmp = smoothdata(vcmd_mmps, 'movmedian', N_vcmd_smooth);

    if N_vcmd_smooth > 7
        vcmd_f = sgolayfilt(vcmd_tmp, 2, N_vcmd_smooth);
    else
        vcmd_f = vcmd_tmp;
    end

    a_cmd_raw = gradient(vcmd_f, t);

    N_acc_cmd_smooth = max(5, round(0.010 / Ts_est));
    if mod(N_acc_cmd_smooth, 2) == 0
        N_acc_cmd_smooth = N_acc_cmd_smooth + 1;
    end

    a_cmd = smoothdata(a_cmd_raw, 'movmedian', N_acc_cmd_smooth);

    %% ---------- 实际加速度 ----------
    a_actual_raw = gradient(v_f, t);

    N_acc_smooth = max(5, round(acc_smooth_time / Ts_est));
    if mod(N_acc_smooth, 2) == 0
        N_acc_smooth = N_acc_smooth + 1;
    end

    a_actual = smoothdata(a_actual_raw, 'movmedian', N_acc_smooth);

    %% ---------- 静态补偿 ----------
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

            [z_lut_um, idx_sort] = sort(z_lut_um);
            i_lut = i_lut(idx_sort);

            z_query_um = z_f * 1000.0 + z_offset_um;

            % 防止外推
            z_query_um = min(max(z_query_um, min(z_lut_um)), max(z_lut_um));

            i_static_mA = interp1(z_lut_um, i_lut, z_query_um, 'pchip');
        end
    else
        i_static_mA = zeros(size(t));
    end

    %% ---------- 残差电流 ----------
    i_res_mA = icmd_f - i_static_mA;

    % 静止段残差检查
    idx_rest = abs(vcmd_f) < 0.1 & abs(v_f) < 0.1;

    if any(idx_rest)
        i_rest_med = median(i_res_mA(idx_rest));
    else
        i_rest_med = NaN;
    end

    fprintf('静止段 Icmd-i_static 中位数 = %.3f mA\n', i_rest_med);

    %% ===================== 4. 识别下行加速段 =====================

    acc_mask = a_cmd > cmd_acc_th;

    if only_down_motion
        acc_mask = acc_mask & (v_f > v_bin_min);
    else
        acc_mask = acc_mask & (abs(v_f) > v_bin_min);
    end

    merge_gap_pts = max(1, round(merge_gap_time / Ts_est));
    acc_mask = merge_short_gaps(acc_mask, merge_gap_pts);

    segments = extract_segments_from_mask(acc_mask, t, 1, min_seg_time, min_seg_pts);

    % 根据 Vcmd 总变化量过滤并裁剪
    N_trim = max(1, round(trim_time / Ts_est));
    segments_refined = [];

    for k = 1:size(segments, 1)

        t1 = segments(k, 1);
        t2 = segments(k, 2);

        idx1 = find(t >= t1, 1, 'first');
        idx2 = find(t <= t2, 1, 'last');

        if isempty(idx1) || isempty(idx2) || idx2 <= idx1
            continue;
        end

        idx1_new = idx1 + N_trim;
        idx2_new = idx2 - N_trim;

        if idx2_new <= idx1_new
            continue;
        end

        delta_vcmd = vcmd_f(idx2_new) - vcmd_f(idx1_new);

        if delta_vcmd < min_vcmd_delta
            continue;
        end

        if (t(idx2_new) - t(idx1_new)) < min_seg_time
            continue;
        end

        if (idx2_new - idx1_new + 1) < min_seg_pts
            continue;
        end

        segments_refined = [segments_refined; ...
            t(idx1_new), t(idx2_new), 1]; %#ok<AGROW>
    end

    segments = segments_refined;

    fprintf('识别到下行加速段数量 = %d\n', size(segments, 1));

    if isempty(segments)
        warning('未识别到有效下行加速段，跳过该文件。');
        continue;
    end

    acc_used = false(size(t));

    for k = 1:size(segments, 1)
        seg = t >= segments(k,1) & t <= segments(k,2);
        acc_used = acc_used | seg;
    end

    %% ===================== 5. 按速度分箱，提取每个文件的统计点 =====================

    v_edges = v_bin_min : v_bin_width : v_bin_max;

    file_bin_rows = [];

    for b = 1:length(v_edges)-1

        v_low = v_edges(b);
        v_high = v_edges(b+1);
        v_mid = 0.5 * (v_low + v_high);

        idx_bin = acc_used & (v_f >= v_low) & (v_f < v_high);

        n_pts = nnz(idx_bin);

        if n_pts < min_bin_pts_per_file
            continue;
        end

        a_med = median(a_actual(idx_bin));
        a_mean = mean(a_actual(idx_bin));

        i_med = median(i_res_mA(idx_bin));
        i_mean = mean(i_res_mA(idx_bin));

        v_med = median(v_f(idx_bin));
        z_med = median(z_f(idx_bin));

        vcmd_med = median(vcmd_f(idx_bin));

        file_bin_rows = [file_bin_rows; ...
            b, v_low, v_high, v_mid, ...
            n_pts, ...
            a_med, a_mean, ...
            i_med, i_mean, ...
            v_med, vcmd_med, z_med]; %#ok<AGROW>
    end

    if isempty(file_bin_rows)
        warning('该文件没有有效速度箱统计点。');
        continue;
    end

    file_bin_table = array2table(file_bin_rows, ...
        'VariableNames', { ...
        'bin_id', 'v_low', 'v_high', 'v_mid', ...
        'n_pts', ...
        'a_med', 'a_mean', ...
        'i_res_med', 'i_res_mean', ...
        'v_med', 'vcmd_med', 'z_med'});

    file_bin_table.file = repmat(string(data_file), height(file_bin_table), 1);
    file_bin_table = movevars(file_bin_table, 'file', 'Before', 1);

    all_bin_points = [all_bin_points; file_bin_table]; %#ok<AGROW>

    file_summary_row = table( ...
        string(data_file), height(file_bin_table), i_rest_med, ...
        'VariableNames', {'file', 'n_valid_bins', 'i_rest_median_mA'});

    all_file_summary = [all_file_summary; file_summary_row]; %#ok<AGROW>

    %% ---------- 单文件绘图 ----------
    if plot_each_file
        figure('Name', ['分段检查 - ', data_file]);

        subplot(4,1,1);
        plot(t, vcmd_f, 'LineWidth', 1.2);
        grid on;
        ylabel('Vcmd / mm/s');
        title(['Vcmd - ', data_file], 'Interpreter', 'none');

        subplot(4,1,2);
        plot(t, a_cmd, 'LineWidth', 1.2);
        grid on;
        ylabel('a\_cmd / mm/s^2');
        title('由 Vcmd 得到的指令加速度');

        subplot(4,1,3);
        plot(t, v_f, 'LineWidth', 1.2); hold on;
        plot(t, acc_used * max(v_f), 'g');
        grid on;
        ylabel('Vel / mm/s');
        legend('Vel', 'acc used');
        title('实际速度与加速段');

        subplot(4,1,4);
        plot(t, i_res_mA, 'LineWidth', 1.2);
        grid on;
        ylabel('Icmd-i\_static / mA');
        xlabel('t / s');
        title('残差电流');
    end
end

%% ===================== 6. 汇总拟合：每个速度箱拟合 i_res = b + ca*a =====================

if isempty(all_bin_points)
    error('没有得到任何有效速度箱统计点，请检查文件名、列号、分段阈值。');
end

v_bins = unique(all_bin_points.bin_id);
fit_rows = [];

fprintf('\n\n====================================================\n');
fprintf('开始按速度箱拟合 i_res = b + ca * a\n');
fprintf('====================================================\n');

for kk = 1:length(v_bins)

    bin_id = v_bins(kk);
    idx = all_bin_points.bin_id == bin_id;

    sub = all_bin_points(idx, :);

    if height(sub) < min_fit_points
        continue;
    end

    a_data = sub.a_med;
    i_data = sub.i_res_med;

    acc_span = max(a_data) - min(a_data);

    if acc_span < min_acc_span
        continue;
    end

    % 线性拟合：i = ca*a + b
    p = polyfit(a_data, i_data, 1);

    ca = p(1);  % mA/(mm/s^2)
    b0 = p(2);  % mA

    i_fit = polyval(p, a_data);

    residual = i_data - i_fit;
    rmse = sqrt(mean(residual.^2));

    SS_res = sum((i_data - i_fit).^2);
    SS_tot = sum((i_data - mean(i_data)).^2);

    if SS_tot > 0
        R2 = 1 - SS_res / SS_tot;
    else
        R2 = NaN;
    end

    if ca > 0
        Kacc = 1 / ca;
    else
        Kacc = NaN;
    end

    valid_fit = isfinite(Kacc) && Kacc >= Kacc_min && Kacc <= Kacc_max;

    v_mid = median(sub.v_mid);
    v_low = median(sub.v_low);
    v_high = median(sub.v_high);

    Kt_N_per_A = 4.12;

    if isfinite(Kacc)
        M_eff = Kt_N_per_A / Kacc;
    else
        M_eff = NaN;
    end

    fit_rows = [fit_rows; ...
        bin_id, v_low, v_high, v_mid, ...
        height(sub), min(a_data), max(a_data), acc_span, ...
        ca, b0, Kacc, M_eff, rmse, R2, double(valid_fit)]; %#ok<AGROW>

    if plot_fit_each_bin
        figure('Name', sprintf('速度箱 %.1f-%.1f mm/s 拟合', v_low, v_high));
        scatter(a_data, i_data, 50, 'filled'); hold on;

        a_line = linspace(min(a_data), max(a_data), 100);
        i_line = polyval(p, a_line);
        plot(a_line, i_line, 'LineWidth', 1.5);

        grid on;
        xlabel('Actual acceleration a / mm/s^2');
        ylabel('i\_res = Icmd - i\_static / mA');
        title(sprintf('速度箱 %.1f-%.1f mm/s: Kacc=%.3f, R^2=%.3f', ...
            v_low, v_high, Kacc, R2));

        text(0.05, 0.95, sprintf('i = %.5f a + %.3f\nKacc = %.3f\nM = %.3f kg', ...
            ca, b0, Kacc, M_eff), ...
            'Units', 'normalized', ...
            'VerticalAlignment', 'top');
    end
end

if isempty(fit_rows)
    error('没有速度箱满足拟合条件，请检查 min_fit_points、min_acc_span 或分段结果。');
end

fit_table = array2table(fit_rows, ...
    'VariableNames', { ...
    'bin_id', 'v_low', 'v_high', 'v_mid', ...
    'n_points', 'a_min', 'a_max', 'a_span', ...
    'ca_mA_per_mmps2', 'intercept_mA', ...
    'Kacc_mmps2_per_mA', 'M_eff_kg', ...
    'rmse_mA', 'R2', 'valid_fit'});

valid_idx = fit_table.valid_fit > 0.5;

if ~any(valid_idx)
    warning('所有速度箱拟合结果都未通过物理范围筛选，将仍然输出全部结果。');
    valid_idx = true(height(fit_table), 1);
end

K_valid = fit_table.Kacc_mmps2_per_mA(valid_idx);

K_median = median(K_valid);
K_mean = mean(K_valid);
K_std = std(K_valid);

weights = fit_table.n_points(valid_idx);
K_weighted = sum(K_valid .* weights) / sum(weights);

Kt_N_per_A = 4.12;
M_eff_median = Kt_N_per_A / K_median;
M_eff_weighted = Kt_N_per_A / K_weighted;

fprintf('\n================ Kacc 汇总结果 ================\n');
fprintf('有效速度箱数量: %d\n', nnz(valid_idx));
fprintf('Kacc 中位数     = %.4f mm/s^2/mA\n', K_median);
fprintf('Kacc 平均值     = %.4f mm/s^2/mA\n', K_mean);
fprintf('Kacc 加权平均值 = %.4f mm/s^2/mA\n', K_weighted);
fprintf('Kacc 标准差     = %.4f mm/s^2/mA\n', K_std);
fprintf('M_eff 中位数估计 = %.4f kg\n', M_eff_median);
fprintf('M_eff 加权估计   = %.4f kg\n', M_eff_weighted);

disp(fit_table);

summary_table = table( ...
    nnz(valid_idx), ...
    K_median, K_mean, K_weighted, K_std, ...
    M_eff_median, M_eff_weighted, ...
    'VariableNames', { ...
    'n_valid_bins', ...
    'Kacc_median', 'Kacc_mean', 'Kacc_weighted', 'Kacc_std', ...
    'M_eff_median_kg', 'M_eff_weighted_kg'});

%% ===================== 7. 总体绘图 =====================

figure('Name', '各速度箱 Kacc');
plot(fit_table.v_mid, fit_table.Kacc_mmps2_per_mA, 'o-', 'LineWidth', 1.2); hold on;
yline(K_median, '--', 'Median');
yline(K_weighted, '--', 'Weighted');
grid on;
xlabel('Velocity bin center / mm/s');
ylabel('Kacc / (mm/s^2/mA)');
title('不同速度箱拟合得到的 Kacc');

figure('Name', '速度箱拟合质量 R2');
plot(fit_table.v_mid, fit_table.R2, 'o-', 'LineWidth', 1.2);
grid on;
xlabel('Velocity bin center / mm/s');
ylabel('R^2');
title('各速度箱 i\_res 与 a 的线性拟合质量');

%% ===================== 8. 保存结果 =====================

writetable(all_bin_points, 'Kacc_multi_acc_bin_points.csv');
writetable(fit_table, 'Kacc_multi_acc_fit_by_speed_bin.csv');
writetable(summary_table, 'Kacc_multi_acc_summary.csv');

fprintf('\n结果已保存：\n');
fprintf('  Kacc_multi_acc_bin_points.csv\n');
fprintf('  Kacc_multi_acc_fit_by_speed_bin.csv\n');
fprintf('  Kacc_multi_acc_summary.csv\n');

%% ============================================================
%  局部函数
%% ============================================================

function mask_out = merge_short_gaps(mask_in, max_gap_pts)
% 合并 mask 中长度不超过 max_gap_pts 的 false 间隙

mask_out = mask_in(:);
N = length(mask_out);

if ~any(mask_out)
    return;
end

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
% 输出：
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