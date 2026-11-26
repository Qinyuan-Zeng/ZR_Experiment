%% batch_estimate_c1_by_xv_bin.m
% ============================================================
% 功能：
%   基于实验三加减速数据，不依赖实验一/实验二模型，
%   通过 (x, v) 分箱去均值方法辨识惯性电流系数 c1。
%
% 基本模型：
%   i = G(x, v) + c1 * a
%
% 其中：
%   G(x, v) 是未知非线性项，包括：
%       弹簧非线性、重力/预紧力、摩擦、滞回、低速非线性等。
%
% 分箱去均值原理：
%   在相近 (x, v) 的小区域内，认为 G(x,v) 近似为常数。
%   对箱内数据去均值：
%       i_tilde = i - mean(i)
%       a_tilde = a - mean(a)
%   则：
%       i_tilde ≈ c1 * a_tilde
%
% 数据格式：
%   第1列：时间
%   第3列：采集速度
%   第4列：指令电流
%   第8列：位置
%
% 文件名格式：
%   2_20_1.csv
%   5_50_2.csv
%   20_400_3.csv
%
% 文件名含义：
%   速度_加速度_重复编号
%
% 作者：
%   根据实验三辨识需求整理
% ============================================================

clear; clc; close all;

%% ============================================================
% 1. 用户配置区
% ============================================================

% 选择实验三数据文件夹
dataDir = uigetdir(pwd, '请选择实验三数据文件夹');

if isequal(dataDir, 0)
    error('未选择数据文件夹，程序终止。');
end

% 支持的文件类型
filePatterns = {'*.csv', '*.txt'};

% ------------------------------------------------------------
% 数据单位设置
% ------------------------------------------------------------

% 第1列时间单位：
% 如果是秒，time_scale = 1
% 如果是毫秒，time_scale = 0.001
time_scale = 1;

% 第8列位置单位：
% 如果已经是 mm，pos_scale = 1
% 如果是编码器脉冲，例如 10000 pulse = 1 mm，则 pos_scale = 1/10000
pos_scale = 1/1000000;

% 第3列速度单位：
% 如果已经是 mm/s，vel_scale = 1
vel_scale = 1;

% 第4列电流单位：
% 如果已经是 A，cur_scale = 1
cur_scale = 1/1000;

% ------------------------------------------------------------
% 方向统一
% ------------------------------------------------------------
% 约定：向下为正方向
%
% 如果压头向下运动时，第8列位置增大，则 pos_sign = 1
% 如果压头向下运动时，第8列位置减小，则 pos_sign = -1
pos_sign = 1;

% 如果向下推力对应的电流为正，则 cur_sign = 1
% 如果向下推力对应的电流为负，则 cur_sign = -1
cur_sign = 1;

% ------------------------------------------------------------
% 速度/加速度计算方式
% ------------------------------------------------------------
% 推荐使用位置离线求导，因为你已经发现采集速度受反馈滤波影响明显。
%
% 'position_sg'：由第8列位置经过 SG 滤波后求速度、加速度，推荐
% 'feedback'   ：速度直接使用第3列采集速度；加速度仍由速度差分得到
velocity_source = 'position_sg';

% ------------------------------------------------------------
% SG 滤波参数
% ------------------------------------------------------------
% 如果采样频率约 1kHz，31 ms 对应约 31 点
sgolay_order = 3;
sgolay_win_sec = 0.031;

% 如果加速度噪声很大，可以调大到 0.041 或 0.051
% 如果动态峰值被明显滤低，可以调小到 0.021
% sgolay_win_sec = 0.021;

% ------------------------------------------------------------
% 分箱参数
% ------------------------------------------------------------
% x-v 分箱宽度
% 这两个参数非常重要，需要根据数据范围微调。
dx_bin = 0.10;      % 位置分箱宽度，单位 mm
dv_bin = 1.00;      % 速度分箱宽度，单位 mm/s

% 每个箱子的最少样本数
minN_bin = 20;

% 每个箱子内加速度变化范围的最小值
% 如果数据最大加速度是 400 mm/s^2，可以先取 30~50
minA_range = 30;    % 单位 mm/s^2

% 剔除速度过零附近数据，避免摩擦换向影响
v_min_abs = 0.5;    % 单位 mm/s

% 异常点剔除百分位
outlier_prc = 99.5;

% 是否按组分别估计 c1
do_group_estimation = true;

% 是否画每个文件的速度对比图
plot_each_file_velocity = false;

% 是否保存图片
save_figures = true;

%% ============================================================
% 2. 搜索所有数据文件
% ============================================================

files = [];

for p = 1:numel(filePatterns)
    tmp = dir(fullfile(dataDir, filePatterns{p}));
    files = [files; tmp]; %#ok<AGROW>
end

if isempty(files)
    error('文件夹中没有找到 csv 或 txt 数据文件。');
end

fprintf('共找到 %d 个数据文件。\n', numel(files));

%% ============================================================
% 3. 批量读取、滤波、求速度和加速度
% ============================================================

allData = table();

for k = 1:numel(files)

    fileName = files(k).name;
    filePath = fullfile(dataDir, fileName);

    [Vset, Aset, repID] = parse_exp3_filename(fileName);

    if isnan(Vset) || isnan(Aset)
        fprintf('跳过无法识别文件名的文件：%s\n', fileName);
        continue;
    end

    raw = readmatrix(filePath);

    if size(raw, 2) < 8
        fprintf('跳过列数不足8列的文件：%s\n', fileName);
        continue;
    end

    % 读取指定列
    t_raw  = raw(:, 1) * time_scale;
    v_fb   = raw(:, 3) * vel_scale;
    i_raw  = raw(:, 4) * cur_scale;
    x_raw  = raw(:, 8) * pos_scale;

    % 去除 NaN 和 Inf
    valid0 = isfinite(t_raw) & isfinite(v_fb) & isfinite(i_raw) & isfinite(x_raw);

    t_raw = t_raw(valid0);
    v_fb  = v_fb(valid0);
    i_raw = i_raw(valid0);
    x_raw = x_raw(valid0);

    if numel(t_raw) < 20
        fprintf('跳过数据点太少的文件：%s\n', fileName);
        continue;
    end

    % 时间从0开始
    t_raw = t_raw - t_raw(1);

    % 统一方向：向下为正
    x_raw = pos_sign * x_raw;
    v_fb  = pos_sign * v_fb;
    i_raw = cur_sign * i_raw;

    % 按时间排序
    [t_raw, idxSort] = sort(t_raw);
    x_raw = x_raw(idxSort);
    v_fb  = v_fb(idxSort);
    i_raw = i_raw(idxSort);

    % 去除重复时间点
    [t_raw, idxUnique] = unique(t_raw, 'stable');
    x_raw = x_raw(idxUnique);
    v_fb  = v_fb(idxUnique);
    i_raw = i_raw(idxUnique);

    if numel(t_raw) < 20
        fprintf('跳过有效时间点太少的文件：%s\n', fileName);
        continue;
    end

    Ts = median(diff(t_raw));

    if Ts <= 0 || ~isfinite(Ts)
        fprintf('跳过时间异常文件：%s\n', fileName);
        continue;
    end

    % 根据采样周期确定 SG 窗口点数
    win = round(sgolay_win_sec / Ts);

    if mod(win, 2) == 0
        win = win + 1;
    end

    win = max(win, sgolay_order + 2);

    if mod(win, 2) == 0
        win = win + 1;
    end

    if win >= numel(x_raw)
        win = numel(x_raw) - 1;
        if mod(win, 2) == 0
            win = win - 1;
        end
    end

    win = max(win, 5);

    % 位置平滑
    x_s = local_smooth_sg(x_raw, sgolay_order, win);

    % 由位置求速度和加速度
    v_pos = gradient(x_s, t_raw);
    v_pos = local_smooth_sg(v_pos, sgolay_order, win);

    a_pos = gradient(v_pos, t_raw);
    a_pos = local_smooth_sg(a_pos, sgolay_order, win);

    % 如果使用反馈速度，则速度取第3列，之后平滑，再求加速度
    switch lower(velocity_source)
        case 'position_sg'
            v_use = v_pos;
            a_use = a_pos;

        case 'feedback'
            v_use = local_smooth_sg(v_fb, sgolay_order, win);
            a_use = gradient(v_use, t_raw);
            a_use = local_smooth_sg(a_use, sgolay_order, win);

        otherwise
            error('velocity_source 只能是 position_sg 或 feedback。');
    end

    % 电流轻微平滑
    i_s = local_smooth_sg(i_raw, sgolay_order, win);

    % 可选：画每个文件的速度对比
    if plot_each_file_velocity
        figure;
        plot(t_raw, v_fb, 'k'); hold on;
        plot(t_raw, v_pos, 'r');
        grid on;
        xlabel('Time / s');
        ylabel('Velocity');
        legend('Feedback velocity', 'Offline velocity');
        title(['速度对比：', fileName], 'Interpreter', 'none');
    end

    N = numel(t_raw);

    Tfile = table();
    Tfile.fileName = repmat({fileName}, N, 1);
    Tfile.Vset = Vset * ones(N, 1);
    Tfile.Aset = Aset * ones(N, 1);
    Tfile.repID = repID * ones(N, 1);
    Tfile.t = t_raw(:);
    Tfile.x_raw = x_raw(:);
    Tfile.x = x_s(:);
    Tfile.v_fb = v_fb(:);
    Tfile.v = v_use(:);
    Tfile.a = a_use(:);
    Tfile.i_raw = i_raw(:);
    Tfile.i = i_s(:);

    allData = [allData; Tfile]; %#ok<AGROW>

    fprintf('已读取：%-20s  V=%g mm/s, A=%g mm/s^2, rep=%g, Ts=%.6g s, win=%d\n', ...
        fileName, Vset, Aset, repID, Ts, win);
end

if isempty(allData)
    error('没有有效数据可处理。');
end

%% ============================================================
% 4. 创建输出文件夹
% ============================================================

outDir = fullfile(dataDir, 'c1_xv_bin_results');

if ~exist(outDir, 'dir')
    mkdir(outDir);
end

%% ============================================================
% 5. 对全部数据进行 c1 估计
% ============================================================

fprintf('\n============================================================\n');
fprintf('开始对全部数据进行 (x,v) 分箱去均值 c1 估计...\n');
fprintf('============================================================\n');

[c1ResultAll, binInfoAll, residualAll] = estimate_c1_xv_bin( ...
    allData, ...
    dx_bin, dv_bin, minN_bin, minA_range, v_min_abs, outlier_prc);

disp(c1ResultAll);

fprintf('\n建议优先查看 c1_robust。\n');
fprintf('c1_robust = %.8g\n', c1ResultAll.c1_robust);

%% ============================================================
% 6. 按速度-加速度组分别估计 c1
% ============================================================

groupSummary = table();

if do_group_estimation

    groupVA = unique([allData.Vset, allData.Aset], 'rows');

    fprintf('\n============================================================\n');
    fprintf('开始按速度-加速度组分别估计 c1...\n');
    fprintf('============================================================\n');

    for g = 1:size(groupVA, 1)

        Vg = groupVA(g, 1);
        Ag = groupVA(g, 2);

        idxG = allData.Vset == Vg & allData.Aset == Ag;

        dataG = allData(idxG, :);

        if height(dataG) < 100
            continue;
        end

        try
            [resG, ~, ~] = estimate_c1_xv_bin( ...
                dataG, ...
                dx_bin, dv_bin, minN_bin, minA_range, v_min_abs, outlier_prc);

            Tg = table(Vg, Ag, resG.num_valid_bins, resG.num_valid_samples, ...
                resG.c1_global, resG.c1_weighted, resG.c1_robust, ...
                'VariableNames', {'Vset','Aset','num_valid_bins','num_valid_samples', ...
                'c1_global','c1_weighted','c1_robust'});

            groupSummary = [groupSummary; Tg]; %#ok<AGROW>

            fprintf('V=%g, A=%g: c1_robust = %.8g, bins=%d, samples=%d\n', ...
                Vg, Ag, resG.c1_robust, resG.num_valid_bins, resG.num_valid_samples);

        catch ME
            fprintf('V=%g, A=%g 组估计失败：%s\n', Vg, Ag, ME.message);
        end
    end

    disp(' ');
    disp('========== 各组 c1 估计结果 ==========');
    disp(groupSummary);
end

%% ============================================================
% 7. 保存数据结果
% ============================================================

writetable(allData, fullfile(outDir, 'processed_all_data.csv'));
writetable(c1ResultAll, fullfile(outDir, 'c1_result_all.csv'));
writetable(binInfoAll, fullfile(outDir, 'c1_bin_info_all.csv'));
writetable(residualAll, fullfile(outDir, 'c1_residual_all.csv'));

if ~isempty(groupSummary)
    writetable(groupSummary, fullfile(outDir, 'c1_group_summary.csv'));
end

fprintf('\n数据结果已保存至：\n%s\n', outDir);

%% ============================================================
% 8. 画图：整体结果诊断
% ============================================================

% 8.1 箱内去均值后 i_res vs a_res
figure;
scatter(residualAll.a_res, residualAll.i_res, 8, 'filled');
grid on;
xlabel('a residual');
ylabel('i residual');
title('箱内去均值后：电流残差 vs 加速度残差');

hold on;
aLine = linspace(min(residualAll.a_res), max(residualAll.a_res), 200);
plot(aLine, c1ResultAll.c1_robust * aLine, 'r', 'LineWidth', 2);
legend('Residual data', 'c1 robust fit', 'Location', 'best');

if save_figures
    saveas(gcf, fullfile(outDir, 'residual_i_vs_a_all.png'));
end

% 8.2 每个分箱的 c1 分布
figure;
histogram(binInfoAll.c1_bin, 30);
grid on;
xlabel('c1 estimated in each (x,v) bin');
ylabel('Count');
title('各 (x,v) 分箱 c1 估计值分布');

if save_figures
    saveas(gcf, fullfile(outDir, 'c1_bin_histogram_all.png'));
end

% 8.3 c1 vs 箱内加速度范围
figure;
scatter(binInfoAll.a_range, binInfoAll.c1_bin, 25, 'filled');
grid on;
xlabel('Acceleration range in bin');
ylabel('c1 in bin');
title('分箱 c1 vs 箱内加速度范围');

if save_figures
    saveas(gcf, fullfile(outDir, 'c1_vs_arange_all.png'));
end

% 8.4 c1 vs 样本数
figure;
scatter(binInfoAll.N, binInfoAll.c1_bin, 25, 'filled');
grid on;
xlabel('Samples in bin');
ylabel('c1 in bin');
title('分箱 c1 vs 样本数');

if save_figures
    saveas(gcf, fullfile(outDir, 'c1_vs_samples_all.png'));
end

% 8.5 按速度-加速度组显示 c1
if ~isempty(groupSummary)

    figure;
    scatter(groupSummary.Aset, groupSummary.c1_robust, 80, groupSummary.Vset, 'filled');
    grid on;
    xlabel('Aset / mm/s^2');
    ylabel('c1 robust');
    title('不同速度-加速度组的 c1 估计结果');
    cb = colorbar;
    ylabel(cb, 'Vset / mm/s');

    if save_figures
        saveas(gcf, fullfile(outDir, 'c1_group_scatter.png'));
    end
end

%% ============================================================
% 9. 输出最终建议
% ============================================================

fprintf('\n============================================================\n');
fprintf('处理完成。\n');
fprintf('============================================================\n');

fprintf('\n总体 c1 结果：\n');
fprintf('  c1_global   = %.8g\n', c1ResultAll.c1_global);
fprintf('  c1_weighted = %.8g\n', c1ResultAll.c1_weighted);
fprintf('  c1_robust   = %.8g  <-- 建议优先采用\n', c1ResultAll.c1_robust);

fprintf('\n有效分箱数：%d\n', c1ResultAll.num_valid_bins);
fprintf('有效去均值样本数：%d\n', c1ResultAll.num_valid_samples);

fprintf('\n如果有效分箱数过少：\n');
fprintf('  1. 增大 dx_bin 或 dv_bin；\n');
fprintf('  2. 降低 minA_range；\n');
fprintf('  3. 检查数据中相近 (x,v) 下是否真的存在不同加速度样本。\n');

fprintf('\n如果各分箱 c1 分布很散：\n');
fprintf('  1. 检查加速度估计是否受滤波影响；\n');
fprintf('  2. 检查电流和位置数据是否同步；\n');
fprintf('  3. 检查高速高加速度组是否存在明显跟踪误差或电流饱和；\n');
fprintf('  4. 可以尝试调整 sgolay_win_sec、dx_bin、dv_bin。\n');

%% ============================================================
% 局部函数 1：文件名解析
% ============================================================

function [Vset, Aset, repID] = parse_exp3_filename(fileName)

    [~, name, ~] = fileparts(fileName);

    % 支持：
    %   2_20_1
    %   5_50_3
    %   20_400
    token = regexp(name, ...
        '^(?<V>[-+]?\d*\.?\d+)_(?<A>[-+]?\d*\.?\d+)(?:_(?<R>\d+))?$', ...
        'names');

    if isempty(token)
        Vset = NaN;
        Aset = NaN;
        repID = NaN;
        return;
    end

    Vset = str2double(token.V);
    Aset = str2double(token.A);

    if isfield(token, 'R') && ~isempty(token.R)
        repID = str2double(token.R);
    else
        repID = NaN;
    end
end

%% ============================================================
% 局部函数 2：SG 平滑
% ============================================================

function y = local_smooth_sg(x, order, win)

    x = x(:);

    if numel(x) < win || win < 5
        y = x;
        return;
    end

    if mod(win, 2) == 0
        win = win + 1;
    end

    if exist('sgolayfilt', 'file') == 2
        y = sgolayfilt(x, order, win);
    else
        % 如果没有 Signal Processing Toolbox，则退化为移动平均
        y = movmean(x, win);
    end
end

%% ============================================================
% 局部函数 3：核心算法——(x,v) 分箱去均值估计 c1
% ============================================================

function [c1Result, binInfo, residualData] = estimate_c1_xv_bin( ...
    dataTable, dx_bin, dv_bin, minN_bin, minA_range, v_min_abs, outlier_prc)

    % ------------------------------------------------------------
    % 1. 取出数据
    % ------------------------------------------------------------

    x = dataTable.x;
    v = dataTable.v;
    a = dataTable.a;
    i = dataTable.i;

    valid = isfinite(x) & isfinite(v) & isfinite(a) & isfinite(i);

    % 剔除速度过零附近，避免摩擦换向影响
    valid = valid & abs(v) > v_min_abs;

    if sum(valid) < 100
        error('有效数据太少。');
    end

    % 剔除异常加速度和电流
    a_lim = prctile(abs(a(valid)), outlier_prc);
    i_lim = prctile(abs(i(valid)), outlier_prc);

    valid = valid & abs(a) < a_lim & abs(i) < i_lim;

    x = x(valid);
    v = v(valid);
    a = a(valid);
    i = i(valid);

    if numel(i) < 100
        error('异常点剔除后有效数据太少。');
    end

    % ------------------------------------------------------------
    % 2. 构造 x-v 分箱
    % ------------------------------------------------------------

    x_min = min(x);
    x_max = max(x);

    v_min_data = min(v);
    v_max_data = max(v);

    x_edges = x_min:dx_bin:x_max;
    v_edges = v_min_data:dv_bin:v_max_data;

    if numel(x_edges) < 2
        x_edges = [x_min, x_max + dx_bin];
    end

    if numel(v_edges) < 2
        v_edges = [v_min_data, v_max_data + dv_bin];
    end

    if x_edges(end) < x_max
        x_edges = [x_edges, x_max + dx_bin];
    end

    if v_edges(end) < v_max_data
        v_edges = [v_edges, v_max_data + dv_bin];
    end

    x_bin = discretize(x, x_edges);
    v_bin = discretize(v, v_edges);

    valid_bin = ~isnan(x_bin) & ~isnan(v_bin);

    x = x(valid_bin);
    v = v(valid_bin);
    a = a(valid_bin);
    i = i(valid_bin);
    x_bin = x_bin(valid_bin);
    v_bin = v_bin(valid_bin);

    [Gid, bx, bv] = findgroups(x_bin, v_bin);

    numBins = max(Gid);

    % ------------------------------------------------------------
    % 3. 箱内去均值
    % ------------------------------------------------------------

    a_res_all = [];
    i_res_all = [];
    gid_all = [];

    c1_bin_list = [];
    w_bin_list = [];
    n_bin_list = [];
    arange_bin_list = [];
    xbin_list = [];
    vbin_list = [];
    xcenter_list = [];
    vcenter_list = [];

    for g = 1:numBins

        idx = (Gid == g);
        n = sum(idx);

        if n < minN_bin
            continue;
        end

        a_g = a(idx);
        i_g = i(idx);
        x_g = x(idx);
        v_g = v(idx);

        a_range = max(a_g) - min(a_g);

        if a_range < minA_range
            continue;
        end

        % 箱内去均值
        a_res = a_g - mean(a_g);
        i_res = i_g - mean(i_g);

        denom = sum(a_res.^2);

        if denom <= eps
            continue;
        end

        % 该箱子独立估计 c1
        c1_g = sum(a_res .* i_res) / denom;

        % 权重：加速度激励强度
        w_g = denom;

        a_res_all = [a_res_all; a_res]; %#ok<AGROW>
        i_res_all = [i_res_all; i_res]; %#ok<AGROW>
        gid_all = [gid_all; g * ones(size(a_res))]; %#ok<AGROW>

        c1_bin_list = [c1_bin_list; c1_g]; %#ok<AGROW>
        w_bin_list = [w_bin_list; w_g]; %#ok<AGROW>
        n_bin_list = [n_bin_list; n]; %#ok<AGROW>
        arange_bin_list = [arange_bin_list; a_range]; %#ok<AGROW>

        xbin_list = [xbin_list; bx(g)]; %#ok<AGROW>
        vbin_list = [vbin_list; bv(g)]; %#ok<AGROW>

        xcenter_list = [xcenter_list; mean(x_g)]; %#ok<AGROW>
        vcenter_list = [vcenter_list; mean(v_g)]; %#ok<AGROW>
    end

    if isempty(c1_bin_list)
        error('没有有效分箱。请增大 dx_bin/dv_bin，或降低 minA_range/minN_bin。');
    end

    % ------------------------------------------------------------
    % 4. 全局 c1 估计
    % ------------------------------------------------------------

    % 方法1：全部去均值样本整体最小二乘
    c1_global = sum(a_res_all .* i_res_all) / sum(a_res_all.^2);

    % 方法2：每个箱子估计值按加速度激励强度加权平均
    c1_weighted = sum(w_bin_list .* c1_bin_list) / sum(w_bin_list);

    % 方法3：鲁棒估计，去掉极端 10% 分箱后加权平均
    q_low = prctile(c1_bin_list, 10);
    q_high = prctile(c1_bin_list, 90);

    keep_robust = c1_bin_list >= q_low & c1_bin_list <= q_high;

    if sum(keep_robust) < 3
        c1_robust = c1_weighted;
    else
        c1_robust = sum(w_bin_list(keep_robust) .* c1_bin_list(keep_robust)) / ...
                    sum(w_bin_list(keep_robust));
    end

    % ------------------------------------------------------------
    % 5. 残差评价
    % ------------------------------------------------------------

    i_res_pred = c1_robust * a_res_all;
    fit_err = i_res_all - i_res_pred;

    rmse = sqrt(mean(fit_err.^2));
    mae = mean(abs(fit_err));

    ss_res = sum(fit_err.^2);
    ss_tot = sum((i_res_all - mean(i_res_all)).^2);

    if ss_tot > eps
        r2 = 1 - ss_res / ss_tot;
    else
        r2 = NaN;
    end

    % ------------------------------------------------------------
    % 6. 输出表格
    % ------------------------------------------------------------

    c1Result = table( ...
        c1_global, c1_weighted, c1_robust, ...
        rmse, mae, r2, ...
        numel(c1_bin_list), numel(i_res_all), ...
        dx_bin, dv_bin, minN_bin, minA_range, v_min_abs, ...
        'VariableNames', { ...
        'c1_global','c1_weighted','c1_robust', ...
        'RMSE','MAE','R2', ...
        'num_valid_bins','num_valid_samples', ...
        'dx_bin','dv_bin','minN_bin','minA_range','v_min_abs'});

    binInfo = table( ...
        c1_bin_list, w_bin_list, n_bin_list, arange_bin_list, ...
        xbin_list, vbin_list, xcenter_list, vcenter_list, ...
        'VariableNames', { ...
        'c1_bin','weight','N','a_range', ...
        'x_bin','v_bin','x_center','v_center'});

    residualData = table( ...
        a_res_all, i_res_all, gid_all, ...
        'VariableNames', {'a_res','i_res','gid'});
end