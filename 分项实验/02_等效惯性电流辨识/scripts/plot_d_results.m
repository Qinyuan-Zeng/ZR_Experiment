function plot_d_results(protocolId, root)
arguments
    protocolId (1,1) string
    root (1,1) string = string(fileparts(fileparts(mfilename('fullpath'))))
end
assert(~isempty(regexp(protocolId,'^Z-DP_\d{8}-\d{6}_\d+_\d+$','once')));
prefix = fullfile(root,"results","d",protocolId);
out = fullfile(root,"data","figures","D",protocolId);
s = jsondecode(fileread(prefix+"_summary.json"));
p = readtable(prefix+"_pairs.csv",TextType="string");
p = p(p.valid==1,:);
assert(~isempty(p),"No valid D pairs to plot.");
p.slot = double(extractAfter(p.planned_pair_id,"_P"));
p = sortrows(p,"slot");
if ~isfolder(out), mkdir(out); end

f = canvas;
a = axes(f);
plot(a,p.slot,p.delta_I_corr_raw,'-o',Color=[0.04 0.29 0.52],LineWidth=1.4,DisplayName='实测修正电流差');
plot(a,p.slot,p.predicted_raw,'--s',Color=[0.65 0.22 0.16],LineWidth=1.4,DisplayName='B 冻结模型预测');
pairAxis(a,s.planned);
ylabel(a,'\fontname{SimSun}电流差（\fontname{Times New Roman}raw\fontname{SimSun}）');
legend(a,Location='best',FontName='SimSun',Box='off');
savePlots(f,out,protocolId+"_01_observed_prediction");

f = canvas;
a = axes(f);
bar(a,p.slot,p.residual_raw,0.52,FaceColor=[0.04 0.29 0.52]);
yline(a,0,'--k');
pairAxis(a,s.planned);
ylabel(a,'\fontname{SimSun}实测减预测（\fontname{Times New Roman}raw\fontname{SimSun}）');
savePlots(f,out,protocolId+"_02_residuals");

f = canvas;
t = tiledlayout(f,1,2,TileSpacing='compact',Padding='compact');
a = nexttile(t);
bar(a,1,abs(s.bias),0.45,FaceColor=[0.04 0.29 0.52]);
yline(a,s.limits.absBias,'--k',sprintf('%.1f raw',s.limits.absBias),FontName='Times New Roman');
axisLabel(a,1,"平均偏差绝对值");
a = nexttile(t);
bar(a,1,s.rmse,0.45,FaceColor=[0.04 0.29 0.52]);
bar(a,2,s.baselineRmse,0.45,FaceColor=[0.55 0.55 0.55]);
yline(a,s.limits.rmse,'--k',sprintf('%.1f raw',s.limits.rmse),FontName='Times New Roman');
axisLabel(a,1:2,["冻结系数","零惯性基线"]);
if s.status == "passed"
    label="通过";
elseif s.status == "failed"
    label="未通过";
else
    label="有效对不足";
end
title(t,sprintf('D 验收：%s（%d/%d）',label,s.accepted,s.planned),FontName='SimSun');
savePlots(f,out,protocolId+"_03_acceptance");
end

function f = canvas
f = figure(Visible='off',Color='w',Position=[100 100 950 470], ...
    DefaultAxesFontName='Times New Roman',DefaultAxesFontSize=13,DefaultAxesBox='on', ...
    DefaultAxesTickDir='in',DefaultAxesYGrid='on',DefaultAxesGridAlpha=0.12,DefaultAxesNextPlot='add');
end

function pairAxis(a,n)
xlim(a,[0.5 n+0.5]); xticks(a,1:n);
xlabel(a,'计划运动对',FontName='SimSun');
end

function axisLabel(a,ticks,labels)
xlim(a,[min(ticks)-0.5 max(ticks)+0.5]); xticks(a,ticks); xticklabels(a,labels);
a.FontName = 'SimSun';
ylabel(a,'raw',FontName='Times New Roman');
end

function savePlots(f,out,name)
savefig(f,fullfile(out,name+".fig"));
exportgraphics(f,fullfile(out,name+".svg"),ContentType='vector');
exportgraphics(f,fullfile(out,name+".png"),Resolution=400);
close(f);
end
