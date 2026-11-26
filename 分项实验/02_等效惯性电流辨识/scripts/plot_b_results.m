function plot_b_results(protocolId, root)
% Use the predeclared protocol output from analyze_b.mjs.
arguments
    protocolId (1,1) string
    root (1,1) string = string(fileparts(fileparts(mfilename('fullpath'))))
end
assert(~isempty(regexp(protocolId,'^Z-BP_[A-Za-z0-9_-]+$','once')));
prefix = fullfile(root,"results","b",protocolId);
out = fullfile(root,"data","figures","B",protocolId);
summary = jsondecode(fileread(prefix+"_summary.json"));
allPairs = readtable(prefix+"_pairs.csv",TextType="string");
p = allPairs(allPairs.valid==1 & allPairs.series=="matrix",:);
assert(~isempty(p),"No accepted matrix pairs to plot.");
if ~isfolder(out), mkdir(out); end
if isfield(summary,"matrixDesign")
    plotExpanded(p,prefix,out,protocolId,summary);
    return
end
labels = ["B1","B2","B3","B4"];
colors = [0.04 0.29 0.52; 0.13 0.48 0.40; 0.78 0.29 0.10; 0.49 0.31 0.59];

f = canvas([1100 700]);
t = tiledlayout(f,2,2,'TileSpacing','compact','Padding','compact');
a = nexttile(t);
scatterConditions(a,p,abs(p.delta_v_pulse_s),labels,colors);
yline(a,500,'--k','500',HandleVisibility='off');
ylim(a,[0 max(550,1.08*max(abs(p.delta_v_pulse_s)))]);
ylabel(a,mix('速度差绝对值','脉冲/s')); headline(a,'实际速度匹配');
a = nexttile(t);
scatterConditions(a,p,abs(p.delta_x_pulse),labels,colors);
yline(a,10,'--k','10',HandleVisibility='off');
ylim(a,[0 max(11,1.08*max(abs(p.delta_x_pulse)))]);
ylabel(a,'位置差绝对值（脉冲）',FontName='SimSun'); headline(a,'实际位置匹配');
a = nexttile(t);
for i=1:numel(labels)
    use = p.condition==labels(i);
    scatter(a,i*ones(nnz(use),1)-0.15,p.accel_a_pulse_s2(use),35,colors(i,:),'o','filled');
    scatter(a,i*ones(nnz(use),1)+0.15,p.decel_a_pulse_s2(use),35,colors(i,:),'s');
end
yline(a,0,':k'); conditionAxis(a,labels);
accLimit = 1.12*max(abs([p.accel_a_pulse_s2;p.decel_a_pulse_s2]));
ylim(a,[-accLimit accLimit]);
ylabel(a,mix('实际加速度','脉冲/s^{2}')); headline(a,'加速圆点与减速方点');
a = nexttile(t);
scatterConditions(a,p,abs(p.delta_a_pulse_s2),labels,colors);
ylabel(a,mix('实际加速度差绝对值','脉冲/s^{2}')); headline(a,'两档激励分布');
saveplots(f,out,protocolId+"_01_matching");

if isempty(summary.fitZero)
    fprintf('Matrix incomplete. Matching plot written. Regression plots await formal data.\n');
    return
end
f = canvas([920 560]);
a = axes(f);
markers = {'o','s','^','d'};
for i=1:numel(labels)
    use=p.condition==labels(i);
    scatter(a,p.delta_a_pulse_s2(use),p.delta_I_corr_raw(use),52,colors(i,:),markers{i},'filled',DisplayName=labels(i));
end
x = linspace(min(p.delta_a_pulse_s2),max(p.delta_a_pulse_s2),200);
z = summary.fitZero;
plot(a,x,z.alpha*x,':k',LineWidth=1.9,DisplayName='b = 0');
if ~isempty(summary.fitFree)
    g = summary.fitFree;
    plot(a,x,g.alpha*x+g.intercept,'-k',LineWidth=1.6,DisplayName='自由截距');
end
xlabel(a,mix('实际加速度差','脉冲/s^{2}')); ylabel(a,mix('位置修正电流差','raw'));
headline(a,'每个独立运动对与联合拟合');
legend(a,'Location','best','FontName','SimSun','Box','off');
saveplots(f,out,protocolId+"_02_joint_regression");

r=readtable(prefix+"_residuals.csv",TextType="string");
metric = "residual_free_raw";
fitTitle = "自由截距";
if isempty(summary.fitFree)
    metric="residual_zero_raw";
    fitTitle="固定截距";
end
f=canvas([1100 425]); t=tiledlayout(f,1,2,'TileSpacing','compact','Padding','compact');
a=nexttile(t); main=r(r.series=="matrix",:);
speeds=sort(unique(main.v_pulse_s));
values=main.(metric);
for i=1:numel(speeds)
    use=main.v_pulse_s==speeds(i);
    scatter(a,repmat(i,nnz(use),1),values(use),37,colors(i,:),'filled');
end
conditionAxis(a,string(speeds)); yline(a,0,':k');
xlabel(a,mix('匹配速度','脉冲/s')); ylabel(a,mix('预测残差','raw'));
headline(a,"H1 主矩阵按速度分组 · "+fitTitle);
a=nexttile(t); h=r(r.series=="history",:);
values=h.(metric);
for i=1:2
    use=h.history_condition==("H"+string(i));
    scatter(a,repmat(i,nnz(use),1),values(use),37,colors(i,:),'filled');
end
conditionAxis(a,["H1","H2"]); yline(a,0,':k');
xlabel(a,'预运动历史',FontName='SimSun'); ylabel(a,mix('相对主矩阵的残差','raw'));
headline(a,"B1 受控历史 · "+fitTitle);
if isempty(h), text(a,1.5,0.5,'暂无有效历史对照数据','HorizontalAlignment','center','FontName','SimSun'); end
saveplots(f,out,protocolId+"_03_residuals");
end

function plotExpanded(p,prefix,out,protocolId,summary)
speeds = sort(unique(p.nominal_v_pulse_s));
colors = [0.04 0.29 0.52; 0.62 0.17 0.12];
f = canvas([1000 600]);
a = axes(f);
for i=1:numel(speeds)
    use = p.nominal_v_pulse_s==speeds(i);
    label = sprintf('%g mm/s',speeds(i)/1000);
    scatter(a,p.delta_a_pulse_s2(use)/1000,p.delta_I_corr_raw(use),32,colors(i,:),'filled', ...
        MarkerFaceAlpha=0.5,HandleVisibility='off');
    group = findgroups(p.nominal_A_pulse_s2(use));
    x = splitapply(@mean,p.delta_a_pulse_s2(use)/1000,group);
    y = splitapply(@mean,p.delta_I_corr_raw(use),group);
    [x,order] = sort(x);
    plot(a,x,y(order),'-o',Color=colors(i,:),LineWidth=1.4,DisplayName=label);
end
if ~isempty(summary.fitZero)
    x = linspace(min(p.delta_a_pulse_s2),max(p.delta_a_pulse_s2));
    plot(a,x/1000,summary.fitZero.alpha*x,':k',LineWidth=1.5,DisplayName='共同系数');
end
xlabel(a,mix('实测加速度差','mm/s^{2}')); ylabel(a,mix('位置修正电流差','raw'));
headline(a,'独立运动对与各档均值');
legend(a,Location='best',FontName='SimSun',Box='off');
saveplots(f,out,protocolId+"_01_acceleration_response");
if isempty(summary.fitZero), return; end

v = readtable(prefix+"_acceleration_validation.csv",TextType="string");
v = v(v.method=="leave_tier_out" & v.training_scope=="pooled",:);
f = canvas([1000 480]);
t = tiledlayout(f,1,2,TileSpacing='compact',Padding='compact');
bias = nexttile(t); rmse = nexttile(t);
for i=1:numel(speeds)
    group = sortrows(v(v.speed_pulse_s==speeds(i),:),"held_out_acc_pulse_s2");
    x = group.held_out_acc_pulse_s2/1000;
    label = sprintf('%g mm/s',speeds(i)/1000);
    plot(bias,x,group.bias_raw,'-o',Color=colors(i,:),DisplayName=label);
    plot(rmse,x,group.rmse_raw,'-o',Color=colors(i,:),DisplayName=label);
end
yline(bias,0,':k',HandleVisibility='off');
for a=[bias rmse]
    xlabel(a,mix('留出档指令加速度','mm/s^{2}'));
    legend(a,Location='best',Box='off');
end
ylabel(bias,mix('平均预测偏差','raw')); ylabel(rmse,mix('预测 RMSE','raw'));
saveplots(f,out,protocolId+"_02_held_out_prediction");
end

function f=canvas(dim)
f=figure('Visible','off','Color','w','Position',[100 100 dim], ...
    'DefaultAxesFontName','Times New Roman','DefaultAxesFontSize',13, ...
    'DefaultAxesBox','on','DefaultAxesTickDir','in','DefaultAxesXGrid','on', ...
    'DefaultAxesYGrid','on','DefaultAxesGridAlpha',0.12,'DefaultAxesNextPlot','add');
end

function scatterConditions(ax,p,value,labels,colors)
for i=1:numel(labels)
    use=p.condition==labels(i);
    scatter(ax,repmat(i,nnz(use),1),value(use),38,colors(i,:),'filled');
end
conditionAxis(ax,labels);
end

function conditionAxis(ax,labels)
xticks(ax,1:numel(labels)); xticklabels(ax,labels);
xlim(ax,[0.5 numel(labels)+0.5]);
end

function label=mix(name,unit)
label=['\fontname{SimSun}' name '（\fontname{Times New Roman}' unit '\fontname{SimSun}）'];
end

function headline(ax,name)
title(ax,name,FontName='SimSun',FontWeight='normal');
end

function saveplots(fig,out,name)
savefig(fig,fullfile(out,name+".fig"));
exportgraphics(fig,fullfile(out,name+".svg"),ContentType='vector');
exportgraphics(fig,fullfile(out,name+".png"),Resolution=400);
close(fig);
end
