%% 实验二：往复步进数据 - 位置负载电流辨识（多模型拟合版）
% 数据文件: 往复5.csv
%
% 目标:
%   1. 从往复步进数据中提取各位置点的稳态保持电流；
%   2. 基于上下行稳态保持电流计算位置相关负载电流 i_load(x)；
%   3. 使用一次、二次、三次多项式和 PCHIP 查表模型拟合 i_load(x)；
%   4. 比较不同模型对电流-位置关系的拟合效果。
%
% 注意:
%   本代码不依赖速度信号，而是通过位置稳定性检测静止段。

clear; close all; clc;

fprintf('=== 实验二：基于位置稳定性的负载电流多模型辨识 ===\n');

%% ===================== 0. 用户配置区 =====================

% 数据文件
filename = '往复5.csv';

% 数据列号，请根据实际 CSV 表头调整
col_time    = 1;     % 时间列，单位 s
col_current = 4;     % 电流指令或实际电流列
col_pos     = 8;     % 编码器位置列，单位 um

% 是否把位置方向取反
% 如果你的编码器数据是"压头向下时位置减小"，则设为 true
% 如果你的编码器数据是"压头向下时位置增大"，则设为 false
reverse_position_sign = false;

% 是否把电流方向取反
% 如果"正电流"不是向下推力，而是向上推力，则设为 true
reverse_current_sign = false;

% 方向判断规则
% 由于本课题约定"向下为正"，因此:
%   若 avg_pos > prev_avg_pos，说明从上一个静止点运动到当前位置时是向下伸出
%   若 avg_pos < prev_avg_pos，说明是向上回缩
%
% 如果你的编码器方向已经通过 reverse_position_sign 调整为"向下为正"，
% 则这里保持为 true。
down_positive_after_preprocess = true;

% 静止段检测参数
window_time = 0.1;               % 滑动窗口时长，单位 s
pos_std_win_threshold = 3.0;     % 窗口内位置标准差阈值，单位 um
min_still_duration = 0.8;        % 最小静止段持续时间，单位 s
stable_duration = 1.0;           % 每个静止段最后多少秒用于计算稳态均值
pos_std_seg_threshold = 2.0;     % 整个静止段位置标准差阈值，单位 um

% 方向判断时，第一个静止段向前查看的时间
pre_check_time = 0.2;            % 单位 s

% 插值位置网格点数
num_grid_points = 500;

% PCHIP 查表模型参数
num_lut_anchor_max = 30;         % PCHIP 查表最大锚点数量
smooth_window_ratio = 50;        % movmean 平滑窗口约为 N / smooth_window_ratio

% 是否保存结果
save_result = true;

%% ===================== 1. 加载数据 =====================

if ~exist(filename, 'file')
    error('未找到数据文件: %s。请确认文件在当前 MATLAB 工作目录下。', filename);
end

data = readtable(filename);

t = data{:, col_time};
i_cmd = data{:, col_current};
pos = data{:, col_pos};

% 转换为列向量
t = t(:);
i_cmd = i_cmd(:);
pos = pos(:);

% 去除 NaN / Inf
valid_raw = isfinite(t) & isfinite(i_cmd) & isfinite(pos);
t = t(valid_raw);
i_cmd = i_cmd(valid_raw);
pos = pos(valid_raw);

% 按时间排序，防止数据乱序
[t, sort_idx] = sort(t);
i_cmd = i_cmd(sort_idx);
pos = pos(sort_idx);

% 预处理方向
if reverse_position_sign
    pos = -pos;
end

if reverse_current_sign
    i_cmd = -i_cmd;
end

% 计算采样周期
dt_vec = diff(t);
dt = median(dt_vec);
fs = 1 / dt;

fprintf('数据加载完成。\n');
fprintf('  文件名: %s\n', filename);
fprintf('  时长: %.3f s\n', t(end) - t(1));
fprintf('  采样数: %d\n', length(t));
fprintf('  估计采样率: %.2f Hz\n', fs);
fprintf('  位置方向取反: %d\n', reverse_position_sign);
fprintf('  电流方向取反: %d\n', reverse_current_sign);

%% ===================== 2. 基于位置稳定性检测静止段 =====================

fprintf('\n正在计算滑动窗口位置标准差...\n');

window_len = max(3, round(window_time * fs));

if mod(window_len, 2) == 0
    window_len = window_len + 1;
end

half_win = floor(window_len / 2);
N = length(pos);
pos_std_win = zeros(N, 1);

for k = 1:N
    idx_start = max(1, k - half_win);
    idx_end   = min(N, k + half_win);
    pos_std_win(k) = std(pos(idx_start:idx_end));
end

% 初步标记潜在静止点
is_potential_still = pos_std_win < pos_std_win_threshold;

% 找连续静止区间
transitions = diff([0; is_potential_still; 0]);
run_starts = find(transitions == 1);
run_ends   = find(transitions == -1) - 1;

still_segments = [];
valid_seg_counter = 0;

for k = 1:length(run_starts)

    seg_start = run_starts(k);
    seg_end   = run_ends(k);

    seg_duration = t(seg_end) - t(seg_start);

    % 条件 1：持续时间足够长
    if seg_duration < min_still_duration
        continue;
    end

    % 条件 2：整个段位置稳定
    pos_seg = pos(seg_start:seg_end);
    pos_std_seg = std(pos_seg);

    if pos_std_seg > pos_std_seg_threshold
        continue;
    end

    % 有效静止段
    valid_seg_counter = valid_seg_counter + 1;
    avg_pos_seg = mean(pos_seg);

    still_segments(valid_seg_counter, :) = ...
        [seg_start, seg_end, seg_duration, avg_pos_seg, pos_std_seg];
end

num_segments = valid_seg_counter;

fprintf('检测到有效静止段数量: %d\n', num_segments);

if num_segments < 3
    error('有效静止段数量过少，请检查静止段检测参数。');
end

%% ===================== 3. 提取每个静止段的稳态电流和位置 =====================

hold_positions = nan(num_segments, 1);
hold_currents  = nan(num_segments, 1);
hold_pos_std   = nan(num_segments, 1);
hold_cur_std   = nan(num_segments, 1);
hold_directions = zeros(num_segments, 1);   % 1: 下行到达, -1: 上行到达, 0: 未知

fprintf('\n正在提取静止段稳态均值...\n');

for seg = 1:num_segments

    seg_start = still_segments(seg, 1);
    seg_end   = still_segments(seg, 2);

    analysis_end_time = t(seg_end);
    analysis_start_time = max(t(seg_start), analysis_end_time - stable_duration);

    analysis_start_idx = find(t >= analysis_start_time, 1, 'first');
    analysis_end_idx = seg_end;

    if isempty(analysis_start_idx) || (analysis_end_idx - analysis_start_idx < 3)
        fprintf('  段 %d: 稳态分析区间过短，跳过。\n', seg);
        continue;
    end

    pos_seg_stable = pos(analysis_start_idx:analysis_end_idx);
    i_seg_stable   = i_cmd(analysis_start_idx:analysis_end_idx);

    avg_pos = mean(pos_seg_stable);
    avg_current = mean(i_seg_stable);

    hold_positions(seg) = avg_pos;
    hold_currents(seg)  = avg_current;
    hold_pos_std(seg)   = std(pos_seg_stable);
    hold_cur_std(seg)   = std(i_seg_stable);

    % 判断方向
    if seg == 1
        % 第一个静止段没有前一个静止点，只能看静止段开始前的位置趋势
        pre_samples = max(2, round(pre_check_time / dt));
        idx_before_start = max(1, seg_start - pre_samples);
        idx_before = idx_before_start:seg_start;

        if length(idx_before) < 2
            direction = 0;
        else
            pos_change_before = pos(seg_start) - pos(idx_before(1));

            if down_positive_after_preprocess
                if pos_change_before > 0
                    direction = 1;      % 向下到达
                elseif pos_change_before < 0
                    direction = -1;     % 向上到达
                else
                    direction = 0;
                end
            else
                if pos_change_before < 0
                    direction = 1;
                elseif pos_change_before > 0
                    direction = -1;
                else
                    direction = 0;
                end
            end
        end

    else
        % 利用前一个静止段平均位置判断到达方向
        prev_avg_pos = hold_positions(seg - 1);

        if ~isfinite(prev_avg_pos)
            direction = 0;
        else
            delta_pos = avg_pos - prev_avg_pos;

            if down_positive_after_preprocess
                if delta_pos > 0
                    direction = 1;      % 从上一点运动到当前位置，位置增大，说明向下到达
                elseif delta_pos < 0
                    direction = -1;     % 位置减小，说明向上到达
                else
                    direction = 0;
                end
            else
                if delta_pos < 0
                    direction = 1;
                elseif delta_pos > 0
                    direction = -1;
                else
                    direction = 0;
                end
            end
        end
    end

    hold_directions(seg) = direction;

    fprintf('  段 %2d: 位置=%10.2f um, 电流=%10.3f, 方向=%2d, 位置std=%.3f, 电流std=%.3f\n', ...
        seg, avg_pos, avg_current, direction, hold_pos_std(seg), hold_cur_std(seg));
end

% 去除无效段
valid_hold = isfinite(hold_positions) & isfinite(hold_currents) & hold_directions ~= 0;

hold_positions  = hold_positions(valid_hold);
hold_currents   = hold_currents(valid_hold);
hold_pos_std    = hold_pos_std(valid_hold);
hold_cur_std    = hold_cur_std(valid_hold);
hold_directions = hold_directions(valid_hold);

fprintf('\n有效稳态保持点数量: %d\n', length(hold_positions));

%% ===================== 4. 区分上下行数据 =====================

idx_down = hold_directions == 1;
idx_up   = hold_directions == -1;

pos_down = hold_positions(idx_down);
i_down   = hold_currents(idx_down);

pos_up = hold_positions(idx_up);
i_up   = hold_currents(idx_up);

fprintf('下行保持点: %d 个\n', length(pos_down));
fprintf('上行保持点: %d 个\n', length(pos_up));

if isempty(pos_down) || isempty(pos_up)
    error('未能提取到足够的下行或上行保持点，请检查方向判断或数据。');
end

%% ===================== 5. 插值到共同位置网格并计算 i_load 与 i_hys =====================

% 排序
[pos_down_sorted, sort_idx_down] = sort(pos_down(:));
i_down_sorted = i_down(sort_idx_down);

[pos_up_sorted, sort_idx_up] = sort(pos_up(:));
i_up_sorted = i_up(sort_idx_up);

% 如果某些位置重复，先按相同位置取平均
[pos_down_unique, ~, gid_down] = unique(pos_down_sorted);
i_down_unique = accumarray(gid_down, i_down_sorted, [], @mean);

[pos_up_unique, ~, gid_up] = unique(pos_up_sorted);
i_up_unique = accumarray(gid_up, i_up_sorted, [], @mean);

% 只取上下行都有覆盖的重叠区间
pos_min_overlap = max(min(pos_down_unique), min(pos_up_unique));
pos_max_overlap = min(max(pos_down_unique), max(pos_up_unique));

if pos_min_overlap >= pos_max_overlap
    error('上下行位置数据没有有效重叠区间，请检查实验数据。');
end

common_pos_grid = linspace(pos_min_overlap, pos_max_overlap, num_grid_points)';

i_down_interp = interp1(pos_down_unique, i_down_unique, common_pos_grid, 'linear');
i_up_interp   = interp1(pos_up_unique,   i_up_unique,   common_pos_grid, 'linear');

valid_interp = isfinite(i_down_interp) & isfinite(i_up_interp);

common_pos_grid = common_pos_grid(valid_interp);
i_down_interp = i_down_interp(valid_interp);
i_up_interp = i_up_interp(valid_interp);

i_load = (i_down_interp + i_up_interp) / 2;
i_hys  = (i_down_interp - i_up_interp) / 2;

fprintf('\n共同位置区间: [%.2f, %.2f] um\n', min(common_pos_grid), max(common_pos_grid));
fprintf('共同网格点数: %d\n', length(common_pos_grid));

%% ===================== 6. 多模型拟合 i_load(x) =====================

x_fit = common_pos_grid(:);
y_fit = i_load(:);

valid_fit = isfinite(x_fit) & isfinite(y_fit);

x_fit = x_fit(valid_fit);
y_fit = y_fit(valid_fit);

% 归一化位置，提高多项式拟合数值稳定性
x_center = mean(x_fit);
x_scale = (max(x_fit) - min(x_fit)) / 2;

if x_scale <= eps
    error('位置范围过小，无法进行拟合。');
end

x_norm = (x_fit - x_center) / x_scale;

% 初始化模型结构
models = struct([]);
model_counter = 0;

% 计算指标函数
calc_metrics = @(y, yhat) struct( ...
    'RMSE', sqrt(mean((y - yhat).^2)), ...
    'MAE', mean(abs(y - yhat)), ...
    'MaxAbs', max(abs(y - yhat)), ...
    'R2', 1 - sum((y - yhat).^2) / sum((y - mean(y)).^2) ...
);

%% 6.1 一次、二次、三次多项式模型
poly_orders = [1, 2, 3];

for kk = 1:length(poly_orders)

    order = poly_orders(kk);

    coeff_norm = polyfit(x_norm, y_fit, order);
    y_hat = polyval(coeff_norm, x_norm);

    model_counter = model_counter + 1;

    models(model_counter).name = sprintf('%d阶多项式', order);
    models(model_counter).type = 'poly';
    models(model_counter).order = order;
    models(model_counter).coeff_norm = coeff_norm;
    models(model_counter).x_center = x_center;
    models(model_counter).x_scale = x_scale;
    models(model_counter).y_hat = y_hat;
    models(model_counter).residual = y_fit - y_hat;
    models(model_counter).metrics = calc_metrics(y_fit, y_hat);

    % 也给出未归一化形式下的系数，方便报告使用
    % 注意：为了避免符号复杂，这里用数值拟合重新获得原始 x 下的系数
    coeff_raw = polyfit(x_fit, y_fit, order);
    models(model_counter).coeff_raw = coeff_raw;
end

%% 6.2 PCHIP 查表模型
num_points = length(x_fit);

% 轻微平滑，避免查表把噪声完全记进去
smooth_window = max(5, round(num_points / smooth_window_ratio));
if mod(smooth_window, 2) == 0
    smooth_window = smooth_window + 1;
end

if smooth_window >= num_points
    smooth_window = max(3, floor(num_points / 5));
    if mod(smooth_window, 2) == 0
        smooth_window = smooth_window + 1;
    end
end

y_smooth = movmean(y_fit, smooth_window);

num_anchor = min(num_lut_anchor_max, num_points);
anchor_idx = unique(round(linspace(1, num_points, num_anchor)));

x_anchor = x_fit(anchor_idx);
y_anchor = y_smooth(anchor_idx);

y_hat_pchip = interp1(x_anchor, y_anchor, x_fit, 'pchip');

model_counter = model_counter + 1;

models(model_counter).name = sprintf('PCHIP查表模型(%d点)', length(x_anchor));
models(model_counter).type = 'pchip_lut';
models(model_counter).order = NaN;
models(model_counter).x_anchor = x_anchor;
models(model_counter).y_anchor = y_anchor;
models(model_counter).smooth_window = smooth_window;
models(model_counter).y_hat = y_hat_pchip;
models(model_counter).residual = y_fit - y_hat_pchip;
models(model_counter).metrics = calc_metrics(y_fit, y_hat_pchip);

%% 6.3 模型指标输出
fprintf('\n===== 负载电流多模型拟合结果 =====\n');
fprintf('%-22s %-12s %-12s %-12s %-12s\n', ...
    '模型', 'RMSE', 'MAE', 'MaxAbs', 'R2');

rmse_list = zeros(length(models), 1);

for kk = 1:length(models)
    rmse_list(kk) = models(kk).metrics.RMSE;

    fprintf('%-22s %-12.4f %-12.4f %-12.4f %-12.5f\n', ...
        models(kk).name, ...
        models(kk).metrics.RMSE, ...
        models(kk).metrics.MAE, ...
        models(kk).metrics.MaxAbs, ...
        models(kk).metrics.R2);
end

[~, best_model_idx] = min(rmse_list);
best_model = models(best_model_idx);

fprintf('\n当前按 RMSE 选择的最优模型: %s\n', best_model.name);
fprintf('  RMSE   = %.4f\n', best_model.metrics.RMSE);
fprintf('  MAE    = %.4f\n', best_model.metrics.MAE);
fprintf('  MaxAbs = %.4f\n', best_model.metrics.MaxAbs);
fprintf('  R2     = %.5f\n', best_model.metrics.R2);

%% ===================== 7. 画图展示结果 =====================

figure('Position', [100, 50, 1300, 900]);

%% 子图1：原始位置信号与滑动窗口标准差
subplot(4, 2, 1);

yyaxis left;
plot(t, pos, 'b-', 'LineWidth', 0.5);
ylabel('位置 (um)');

yyaxis right;
plot(t, pos_std_win, 'r-', 'LineWidth', 0.5);
ylabel('窗口位置标准差 (um)');

xlabel('时间 (s)');
title('原始位置信号与滑动窗口标准差');
grid on;
legend('位置', '位置标准差(窗口)', 'Location', 'best');

%% 子图2：原始电流信号与检测到的静止段
subplot(4, 2, 2);

plot(t, i_cmd, 'k-', 'LineWidth', 0.5);
ylabel('电流 Icmd');
xlabel('时间 (s)');
title('原始电流信号与检测到的静止段');
grid on;
hold on;

yl = ylim;

for seg = 1:num_segments
    seg_start = still_segments(seg, 1);
    seg_end = still_segments(seg, 2);

    x_patch = [t(seg_start), t(seg_end), t(seg_end), t(seg_start)];
    y_patch = [yl(1), yl(1), yl(2), yl(2)];

    patch(x_patch, y_patch, [0.9 0.7 0.7], ...
        'EdgeColor', 'none', ...
        'FaceAlpha', 0.25, ...
        'HandleVisibility', 'off');
end

plot(t, i_cmd, 'k-', 'LineWidth', 0.5);

% 标记有效稳态点
if ~isempty(hold_positions)
    % 重新找有效段的结束时间
    valid_seg_indices = find(valid_hold);
    valid_seg_end_idx = still_segments(valid_seg_indices, 2);
    scatter(t(valid_seg_end_idx), hold_currents, 35, 'g', 'filled', ...
        'DisplayName', '稳态保持电流');
end

hold off;
legend('电流', '稳态保持电流', 'Location', 'best');

%% 子图3：上下行稳态保持电流散点
subplot(4, 2, 3);

scatter(pos_down, i_down, 70, 'b^', 'filled', ...
    'DisplayName', '下行保持电流');
hold on;

scatter(pos_up, i_up, 70, 'rv', 'filled', ...
    'DisplayName', '上行保持电流');

xlabel('位置 (um)');
ylabel('保持电流');
title('上下行稳态保持电流散点图');
grid on;
legend('Location', 'best');

%% 子图4：负载电流多模型拟合与滞回电流
subplot(4, 2, 4);

yyaxis left;
plot(x_fit, y_fit, 'g-', 'LineWidth', 2, ...
    'DisplayName', 'i_{load}(计算)');
hold on;

line_styles = {'m--', 'b-.', 'r:', 'k-'};

for kk = 1:length(models)
    style_idx = mod(kk - 1, length(line_styles)) + 1;

    plot(x_fit, models(kk).y_hat, line_styles{style_idx}, ...
        'LineWidth', 1.6, ...
        'DisplayName', sprintf('%s, R^2=%.4f', ...
        models(kk).name, models(kk).metrics.R2));
end

ylabel('i_{load} (负载电流)');

yyaxis right;
plot(common_pos_grid, i_hys, 'c-', ...
    'LineWidth', 1.2, ...
    'DisplayName', 'i_{hys}');

ylabel('i_{hys} (滞回电流)');
xlabel('位置 (um)');
title('负载电流多模型拟合与滞回电流');
grid on;
legend('Location', 'best');

%% 子图5：不同模型拟合残差
subplot(4, 2, 5);

hold on;

for kk = 1:length(models)
    style_idx = mod(kk - 1, length(line_styles)) + 1;

    plot(x_fit, models(kk).residual, line_styles{style_idx}, ...
        'LineWidth', 1.2, ...
        'DisplayName', sprintf('%s残差', models(kk).name));
end

yline(0, 'r--', 'LineWidth', 1);
xlabel('位置 (um)');
ylabel('残差');
title('不同模型的负载电流拟合残差');
grid on;
legend('Location', 'best');

%% 子图6：滞回电流 vs 位置
subplot(4, 2, 6);

plot(common_pos_grid, i_hys, 'b-', 'LineWidth', 1.5);
xlabel('位置 (um)');
ylabel('i_{hys}');
title('滞回电流 vs 位置');
grid on;

avg_hys = mean(i_hys);
min_hys = min(i_hys);
max_hys = max(i_hys);

hold on;
yline(avg_hys, 'r--', sprintf('均值: %.3f', avg_hys));
legend('滞回电流', sprintf('均值=%.3f', avg_hys), 'Location', 'best');

%% 子图7：静止段持续时间分布
subplot(4, 2, 7);

histogram(still_segments(:, 3), 20, ...
    'FaceColor', [0.2 0.6 0.8]);

xlabel('静止段持续时间 (s)');
ylabel('频次');
title('静止段持续时间分布');
grid on;

%% 子图8：静止段位置序列
subplot(4, 2, 8);

plot(1:num_segments, still_segments(:, 4), 'bo-', ...
    'LineWidth', 1, ...
    'MarkerFaceColor', 'b');

xlabel('静止段序号');
ylabel('平均位置 (um)');
title('静止段位置序列');
grid on;

sgtitle('实验二：基于位置稳定性的负载电流多模型辨识', ...
    'FontSize', 14, ...
    'FontWeight', 'bold');

%% ===================== 8. 模型评价指标对比图 =====================

figure('Position', [150, 150, 1100, 430]);

model_names = cell(length(models), 1);
rmse_values = zeros(length(models), 1);
maxabs_values = zeros(length(models), 1);
r2_values = zeros(length(models), 1);

for kk = 1:length(models)
    model_names{kk} = models(kk).name;
    rmse_values(kk) = models(kk).metrics.RMSE;
    maxabs_values(kk) = models(kk).metrics.MaxAbs;
    r2_values(kk) = models(kk).metrics.R2;
end

subplot(1, 3, 1);
bar(rmse_values);
set(gca, 'XTick', 1:length(models), ...
    'XTickLabel', model_names, ...
    'XTickLabelRotation', 25);
ylabel('RMSE');
title('RMSE 对比');
grid on;

subplot(1, 3, 2);
bar(maxabs_values);
set(gca, 'XTick', 1:length(models), ...
    'XTickLabel', model_names, ...
    'XTickLabelRotation', 25);
ylabel('最大绝对误差');
title('最大误差对比');
grid on;

subplot(1, 3, 3);
bar(r2_values);
set(gca, 'XTick', 1:length(models), ...
    'XTickLabel', model_names, ...
    'XTickLabelRotation', 25);
ylabel('R^2');
title('拟合优度 R^2 对比');
grid on;

sgtitle('负载电流模型拟合指标对比');

%% ===================== 9. 输出关键结果 =====================

fprintf('\n===== 处理结果摘要 =====\n');

fprintf('\n1. 一次多项式模型，原始 x 形式:\n');
fprintf('   i_load(x) = %.8e * x + %.6f\n', ...
    models(1).coeff_raw(1), models(1).coeff_raw(2));
fprintf('   RMSE = %.4f, R2 = %.5f\n', ...
    models(1).metrics.RMSE, models(1).metrics.R2);

fprintf('\n2. 二次多项式模型，原始 x 形式:\n');
fprintf('   i_load(x) = %.8e * x^2 + %.8e * x + %.6f\n', ...
    models(2).coeff_raw(1), models(2).coeff_raw(2), models(2).coeff_raw(3));
fprintf('   RMSE = %.4f, R2 = %.5f\n', ...
    models(2).metrics.RMSE, models(2).metrics.R2);

fprintf('\n3. 三次多项式模型，原始 x 形式:\n');
fprintf('   i_load(x) = %.8e * x^3 + %.8e * x^2 + %.8e * x + %.6f\n', ...
    models(3).coeff_raw(1), models(3).coeff_raw(2), ...
    models(3).coeff_raw(3), models(3).coeff_raw(4));
fprintf('   RMSE = %.4f, R2 = %.5f\n', ...
    models(3).metrics.RMSE, models(3).metrics.R2);

fprintf('\n4. PCHIP 查表模型:\n');
fprintf('   锚点数量: %d\n', length(models(4).x_anchor));
fprintf('   平滑窗口: %d 点\n', models(4).smooth_window);
fprintf('   RMSE = %.4f, R2 = %.5f\n', ...
    models(4).metrics.RMSE, models(4).metrics.R2);

fprintf('\n推荐模型:\n');
fprintf('   当前按 RMSE 选择: %s\n', best_model.name);

fprintf('\n滞回电流统计:\n');
fprintf('   平均值: %.4f\n', avg_hys);
fprintf('   范围: [%.4f, %.4f]\n', min_hys, max_hys);

fprintf('\n静止段位置序列:\n');
disp(still_segments(:, 4)');

%% ===================== 10. 保存结果 =====================

if save_result

    load_model_results = struct();

    load_model_results.filename = filename;

    load_model_results.raw.t = t;
    load_model_results.raw.pos = pos;
    load_model_results.raw.i_cmd = i_cmd;
    load_model_results.raw.pos_std_win = pos_std_win;

    load_model_results.config.reverse_position_sign = reverse_position_sign;
    load_model_results.config.reverse_current_sign = reverse_current_sign;
    load_model_results.config.down_positive_after_preprocess = down_positive_after_preprocess;
    load_model_results.config.window_time = window_time;
    load_model_results.config.pos_std_win_threshold = pos_std_win_threshold;
    load_model_results.config.min_still_duration = min_still_duration;
    load_model_results.config.stable_duration = stable_duration;
    load_model_results.config.pos_std_seg_threshold = pos_std_seg_threshold;

    load_model_results.still_segments = still_segments;

    load_model_results.hold.positions = hold_positions;
    load_model_results.hold.currents = hold_currents;
    load_model_results.hold.directions = hold_directions;
    load_model_results.hold.pos_down = pos_down;
    load_model_results.hold.i_down = i_down;
    load_model_results.hold.pos_up = pos_up;
    load_model_results.hold.i_up = i_up;

    load_model_results.grid.common_pos_grid = common_pos_grid;
    load_model_results.grid.i_down_interp = i_down_interp;
    load_model_results.grid.i_up_interp = i_up_interp;
    load_model_results.grid.i_load = i_load;
    load_model_results.grid.i_hys = i_hys;

    load_model_results.fit.x_fit = x_fit;
    load_model_results.fit.y_fit = y_fit;
    load_model_results.fit.x_center = x_center;
    load_model_results.fit.x_scale = x_scale;

    load_model_results.models = models;
    load_model_results.best_model_idx = best_model_idx;
    load_model_results.best_model = best_model;

    save('load_current_multi_model_results.mat', 'load_model_results');

    fprintf('\n多模型负载电流拟合结果已保存到: load_current_multi_model_results.mat\n');

    % 同时保存一份 CSV
    csv_table = table( ...
        x_fit(:), ...
        y_fit(:), ...
        models(1).y_hat(:), ...
        models(2).y_hat(:), ...
        models(3).y_hat(:), ...
        models(4).y_hat(:), ...
        i_hys(:), ...
        'VariableNames', { ...
            'Position_um', ...
            'i_load_calc', ...
            'i_load_poly1', ...
            'i_load_poly2', ...
            'i_load_poly3', ...
            'i_load_pchip', ...
            'i_hys' ...
        });

    writetable(csv_table, 'load_current_multi_model_comparison.csv');

    fprintf('多模型对比数据已保存到: load_current_multi_model_comparison.csv\n');
end

fprintf('\n=== 分析完成 ===\n');

%% ===================== 11. 后续调用示例 =====================
%{
% 示例 1：调用二次模型计算某位置的负载电流
x_query = 8000;   % um
model_quad = load_model_results.models(2);
i_load_quad = polyval(model_quad.coeff_raw, x_query);

% 示例 2：调用 PCHIP 查表模型计算某位置的负载电流
model_pchip = load_model_results.models(4);
i_load_pchip = interp1(model_pchip.x_anchor, model_pchip.y_anchor, x_query, 'pchip', 'extrap');

fprintf('x = %.2f um 时，二次模型 i_load = %.4f\n', x_query, i_load_quad);
fprintf('x = %.2f um 时，PCHIP 模型 i_load = %.4f\n', x_query, i_load_pchip);
%}