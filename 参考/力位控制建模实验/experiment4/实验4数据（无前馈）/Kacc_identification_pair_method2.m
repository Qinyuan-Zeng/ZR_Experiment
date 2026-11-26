%% ============================================================
%  加减速配对法辨识 Kacc
%
%  思路：
%    1. 使用 Vcmd 第13列识别加速段和减速段；
%    2. 使用实际速度 Vel 第3列计算实际加速度；
%    3. 使用 Icmd 第4列减去静态补偿电流：
%           i_res = Icmd - i_static(z)
%    4. 在相同实际速度区间内配对：
%           Kacc = (a_acc - a_dec) / (i_res_acc - i_res_dec)
%
%  单位：
%    Vel: mm/s
%    a: mm/s^2
%    Icmd, i_static: mA
%    Kacc: mm/s^2/mA
%
%  数据列号按你的表头：
%    第1列  time(s)
%    第3列  Vel
%    第4列  Icmd
%    第8列  Encoder ACC Feedback，位移
%    第13列 Vcmd
%% ============================================================

clear; clc; close all;

%% ===================== 1. 用户参数区 =====================

% ---------- 文件设置 ----------
% 单文件处理：
data_files = {
    '40_400_1.csv'
};

% 如果想批量处理当前文件夹下所有 csv，可以改用：
% d = dir('*.csv');
% data_files = {d.name};
% data_files = data_files(~contains(data_files, 'Kacc_'));
% data_files = data_files(~contains(data_files, 'pchip'));

% ---------- 列号设置 ----------
time_col = 1;      % time(s)
vel_col  = 3;      % Vel，实际速度
icmd_col = 4;      % Icmd，指令电流
z_col    = 8;      % Encoder ACC Feedback，实际位移
vcmd_col = 13;     % Vcmd，指令速度

% ---------- 单位设置 ----------
z_unit = 'um';       % 第8列如果是 um，保持 um；如果是 mm，改成 mm
v_unit = 'mmps';     % 速度单位，mm/s
i_unit = 'mA';       % 电流单位，mA

% ---------- 静态补偿表 ----------
use_static_lut = true;

static_lut_file = 'best_down_pchip_lut_anchor_points.csv';
static_pos_col = 1;
static_cur_col = 2;
static_pos_unit = 'um';

% 如果当前实验位移零点和静态表零点不一致，在这里加偏置
% 例如当前实验 z=0 对应静态表 z=1000um，则设 z_offset_um=1000
z_offset_um = 0.0;

% ---------- 滤波参数 ----------
use_filter = true;

% 实际速度、电流、位移滤波
sgolay_order = 3;
sgolay_frame = 51;          % 8000 Hz 数据可用 51/101

% Vcmd 平滑后求导
vcmd_smooth_time = 0.020;   % s，建议 0.020~0.030

% 实际速度平滑后求实际加速度
acc_smooth_time = 0.010;    % s

% ---------- 加减速段识别参数 ----------
cmd_acc_th = 80.0;          % mm/s^2，Vcmd 导数阈值
merge_gap_time = 0.020;     % s，合并 mask 中短断裂
min_seg_time = 0.050;       % s，片段最短时间
min_seg_pts  = 100;         % 片段最少点数
min_vcmd_delta = 5.0;       % mm/s，片段内 Vcmd 最小变化量
trim_time = 0.005;          % s，片段边界裁剪

% 只分析下行
% 如果下行速度为正，设 true
only_down_motion = true;

% ---------- 速度分箱配对参数 ----------
v_bin_min = 5.0;            % mm/s
v_bin_max = 35.0;           % mm/s
v_bin_width = 5.0;          % mm/s

min_bin_pts = 30;           % 每个速度箱内加速/减速段至少点数
min_current_diff = 5.0;     % mA，电流差过小则该速度箱无效

% ---------- 电流方向 ----------
% 如果算出来大量 Kacc 为负，尝试改成 -1
current_sign = 1;

% ---------- 绘图 ----------
plot_each_file = true;


%% ===================== 2. 汇总结果表 =====================

all_bin_results = table();
all_summary = table();


%% ===================== 3. 循环处理每个文件 =====================

for file_id = 1:numel(data_files)

    data_file = data_files{file_id};
    fprintf('\n\n====================================================\n');
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

    %% ---------- Vcmd 平滑与指令加速度 ----------
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

    %% ---------- 电流残差 ----------
    i_res_mA = icmd_f - i_static_mA;

    % 静止残差检查
    idx_rest = abs(vcmd_f) < 0.1 & abs(v_f) < 0.1;

    if any(idx_rest)
        i_rest_med = median(i_res_mA(idx_rest));
    else
        i_rest_med = NaN;
    end

    fprintf('静止段 Icmd-i_static 中位数 = %.3f mA\n', i_rest_med);

    %% ===================== 4. 识别加速段和减速段 =====================

    acc_mask = a_cmd > cmd_acc_th;
    dec_mask = a_cmd < -cmd_acc_th;

    if only_down_motion
        acc_mask = acc_mask & (v_f > v_bin_min);
        dec_mask = dec_mask & (v_f > v_bin_min);
    else
        acc_mask = acc_mask & (abs(v_f) > v_bin_min);
        dec_mask = dec_mask & (abs(v_f) > v_bin_min);
    end

    merge_gap_pts = max(1, round(merge_gap_time / Ts_est));
    acc_mask = merge_short_gaps(acc_mask, merge_gap_pts);
    dec_mask = merge_short_gaps(dec_mask, merge_gap_pts);

    segments = [];
    segments = [segments; extract_segments_from_mask(acc_mask, t,  1, min_seg_time, min_seg_pts)];
    segments = [segments; extract_segments_from_mask(dec_mask, t, -1, min_seg_time, min_seg_pts)];

    % 根据 Vcmd 总变化量过滤并裁剪
    N_trim = max(1, round(trim_time / Ts_est));
    segments_refined = [];

    for k = 1:size(segments, 1)

        t1 = segments(k, 1);
        t2 = segments(k, 2);
        seg_type = segments(k, 3);

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

        if seg_type == 1
            if delta_vcmd < min_vcmd_delta
                continue;
            end
        else
            if delta_vcmd > -min_vcmd_delta
                continue;
            end
        end

        if (t(idx2_new) - t(idx1_new)) < min_seg_time
            continue;
        end

        if (idx2_new - idx1_new + 1) < min_seg_pts
            continue;
        end

        segments_refined = [segments_refined; ...
            t(idx1_new), t(idx2_new), seg_type]; %#ok<AGROW>
    end

    segments = segments_refined;

    if ~isempty(segments)
        [~, idx_sort] = sort(segments(:,1));
        segments = segments(idx_sort, :);
    end

    fprintf('识别到加减速片段数量 = %d\n', size(segments, 1));

    if isempty(segments)
        warning('未识别到有效加减速片段，跳过该文件。');
        continue;
    end

    %% ---------- 生成最终使用的 acc_used / dec_used mask ----------
    acc_used = false(size(t));
    dec_used = false(size(t));

    for k = 1:size(segments, 1)
        seg = t >= segments(k,1) & t <= segments(k,2);

        if segments(k,3) == 1
            acc_used = acc_used | seg;
        else
            dec_used = dec_used | seg;
        end
    end

    %% ===================== 5. 速度分箱配对计算 Kacc =====================

    v_edges = v_bin_min : v_bin_width : v_bin_max;

    bin_rows = [];

    for b = 1:length(v_edges)-1

        v1 = v_edges(b);
        v2 = v_edges(b+1);
        v_mid = 0.5 * (v1 + v2);

        idx_acc = acc_used & (v_f >= v1) & (v_f < v2);
        idx_dec = dec_used & (v_f >= v1) & (v_f < v2);

        n_acc = nnz(idx_acc);
        n_dec = nnz(idx_dec);

        if n_acc < min_bin_pts || n_dec < min_bin_pts
            continue;
        end

        a_acc_med = median(a_actual(idx_acc));
        a_dec_med = median(a_actual(idx_dec));

        i_acc_med = median(i_res_mA(idx_acc));
        i_dec_med = median(i_res_mA(idx_dec));

        z_acc_med = median(z_f(idx_acc));
        z_dec_med = median(z_f(idx_dec));

        v_acc_med = median(v_f(idx_acc));
        v_dec_med = median(v_f(idx_dec));

        numerator = a_acc_med - a_dec_med;
        denominator = i_acc_med - i_dec_med;

        if abs(denominator) < min_current_diff
            Kacc_bin = NaN;
            valid_bin = false;
        else
            Kacc_bin = numerator / denominator;
            valid_bin = isfinite(Kacc_bin) && Kacc_bin > 0;
        end

        bin_rows = [bin_rows; ...
            b, v1, v2, v_mid, ...
            n_acc, n_dec, ...
            v_acc_med, v_dec_med, ...
            z_acc_med, z_dec_med, z_acc_med - z_dec_med, ...
            a_acc_med, a_dec_med, ...
            i_acc_med, i_dec_med, ...
            numerator, denominator, Kacc_bin, double(valid_bin)]; %#ok<AGROW>
    end

    if isempty(bin_rows)
        warning('没有得到有效速度分箱结果。');
        continue;
    end

    bin_table = array2table(bin_rows, ...
        'VariableNames', { ...
        'bin_id', 'v_low', 'v_high', 'v_mid', ...
        'n_acc', 'n_dec', ...
        'v_acc_med', 'v_dec_med', ...
        'z_acc_med', 'z_dec_med', 'z_diff_acc_dec', ...
        'a_acc_med', 'a_dec_med', ...
        'i_acc_med', 'i_dec_med', ...
        'a_diff', 'i_diff', 'Kacc', 'valid_bin'});

    % 添加文件名列
    bin_table.file = repmat(string(data_file), height(bin_table), 1);
    bin_table = movevars(bin_table, 'file', 'Before', 1);

    valid_idx = bin_table.valid_bin > 0.5;
    K_valid = bin_table.Kacc(valid_idx);

    if isempty(K_valid)
        warning('速度分箱有结果，但没有正的有效 Kacc。');
        continue;
    end

    K_median = median(K_valid);
    K_mean = mean(K_valid);
    K_std = std(K_valid);

    weights = bin_table.n_acc(valid_idx) + bin_table.n_dec(valid_idx);
    K_weighted = sum(K_valid .* weights) / sum(weights);

    Kt_N_per_A = 4.12;
    M_eff_median = Kt_N_per_A / K_median;
    M_eff_weighted = Kt_N_per_A / K_weighted;

    fprintf('\n=========== 文件 %s 的 Kacc 结果 ===========\n', data_file);
    fprintf('有效速度箱数量: %d\n', length(K_valid));
    fprintf('Kacc 中位数     = %.4f mm/s^2/mA\n', K_median);
    fprintf('Kacc 平均值     = %.4f mm/s^2/mA\n', K_mean);
    fprintf('Kacc 加权平均值 = %.4f mm/s^2/mA\n', K_weighted);
    fprintf('Kacc 标准差     = %.4f mm/s^2/mA\n', K_std);
    fprintf('M_eff 中位数估计 = %.4f kg\n', M_eff_median);
    fprintf('M_eff 加权估计   = %.4f kg\n', M_eff_weighted);

    disp(bin_table);

    % 汇总
    summary_row = table( ...
        string(data_file), length(K_valid), ...
        K_median, K_mean, K_weighted, K_std, ...
        M_eff_median, M_eff_weighted, i_rest_med, ...
        'VariableNames', { ...
        'file', 'n_valid_bins', ...
        'Kacc_median', 'Kacc_mean', 'Kacc_weighted', 'Kacc_std', ...
        'M_eff_median_kg', 'M_eff_weighted_kg', 'i_rest_median_mA'});

    all_bin_results = [all_bin_results; bin_table]; %#ok<AGROW>
    all_summary = [all_summary; summary_row]; %#ok<AGROW>

    %% ===================== 6. 绘图检查 =====================

    if plot_each_file

        figure('Name', ['分段检查 - ', data_file]);

        subplot(4,1,1);
        plot(t, vcmd_f, 'LineWidth', 1.2);
        grid on;
        ylabel('Vcmd / mm/s');
        title(['Vcmd，第13列 - ', data_file], 'Interpreter', 'none');

        subplot(4,1,2);
        plot(t, a_cmd, 'LineWidth', 1.2);
        grid on;
        ylabel('a\_cmd / mm/s^2');
        title('由 Vcmd 得到的指令加速度');

        subplot(4,1,3);
        plot(t, v_f, 'LineWidth', 1.2); hold on;
        plot(t, acc_used * max(v_f), 'g');
        plot(t, dec_used * max(v_f), 'r');
        grid on;
        ylabel('Vel / mm/s');
        legend('Vel', 'acc used', 'dec used');
        title('实际速度与筛选片段');

        subplot(4,1,4);
        plot(t, i_res_mA, 'LineWidth', 1.2);
        grid on;
        ylabel('Icmd-i\_static / mA');
        xlabel('t / s');
        title('电流残差');

        figure('Name', ['实际速度与电流残差 - ', data_file]);

        yyaxis left;
        plot(t, v_f, 'LineWidth', 1.2);
        ylabel('Actual Vel / mm/s');

        yyaxis right;
        plot(t, i_res_mA, 'LineWidth', 1.2);
        ylabel('Icmd - i\_static / mA');

        grid on;
        xlabel('t / s');
        title(['实际速度与电流残差 - ', data_file], 'Interpreter', 'none');

        hold on;
        for k = 1:size(segments, 1)
            if segments(k,3) > 0
                xline(segments(k,1), '--g', 'acc start');
                xline(segments(k,2), '--g', 'acc end');
            else
                xline(segments(k,1), '--r', 'dec start');
                xline(segments(k,2), '--r', 'dec end');
            end
        end

        figure('Name', ['速度分箱 Kacc - ', data_file]);
        plot(bin_table.v_mid(valid_idx), bin_table.Kacc(valid_idx), 'o-', 'LineWidth', 1.2); hold on;
        yline(K_median, '--', 'Median');
        yline(K_weighted, '--', 'Weighted');
        grid on;
        xlabel('Velocity bin center / mm/s');
        ylabel('Kacc / (mm/s^2/mA)');
        title(['速度分箱 Kacc - ', data_file], 'Interpreter', 'none');

        figure('Name', ['配对电流与加速度差 - ', data_file]);

        subplot(2,1,1);
        plot(bin_table.v_mid, bin_table.a_diff, 'o-', 'LineWidth', 1.2);
        grid on;
        xlabel('Velocity bin center / mm/s');
        ylabel('a\_acc - a\_dec / mm/s^2');
        title('加减速加速度差');

        subplot(2,1,2);
        plot(bin_table.v_mid, bin_table.i_diff, 'o-', 'LineWidth', 1.2);
        grid on;
        xlabel('Velocity bin center / mm/s');
        ylabel('i\_acc - i\_dec / mA');
        title('加减速电流残差差');
    end
end


%% ===================== 7. 保存总结果 =====================

if ~isempty(all_bin_results)
    writetable(all_bin_results, 'Kacc_pair_method_bin_results.csv');
    fprintf('\n已保存：Kacc_pair_method_bin_results.csv\n');
end

if ~isempty(all_summary)
    writetable(all_summary, 'Kacc_pair_method_summary.csv');
    fprintf('已保存：Kacc_pair_method_summary.csv\n');

    fprintf('\n================ 所有文件汇总 ================\n');
    disp(all_summary);

    fprintf('\n总体 Kacc 统计：\n');
    fprintf('各文件 Kacc_weighted 中位数 = %.4f mm/s^2/mA\n', median(all_summary.Kacc_weighted));
    fprintf('各文件 Kacc_weighted 平均值 = %.4f mm/s^2/mA\n', mean(all_summary.Kacc_weighted));
end


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