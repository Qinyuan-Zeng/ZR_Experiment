function plot_formal_a_half_difference
% 正式 A 的完整往返；每趟已扣除位置项及惯性项。五轮分别取自两个现场会话。
root = fileparts(fileparts(mfilename('fullpath')));
id = "Z-AF-20261126-03";
t = readtable(fullfile(root,"results","formal_a",id,"pairs.csv"));
speeds = unique(t.speed);
assert(height(t)==85 && numel(speeds)==17 && isequal(unique(t.round),(1:5)'));
current = zeros(17,5);
for r = 1:5
    trial = sortrows(t(t.round==r,:), "speed");
    assert(isequal(trial.speed,speeds) && all(isfinite(trial.half_diff_raw)));
    current(:,r) = trial.half_diff_raw * 1.2; % a33, 1.200 Arms: mA/raw
end

f = figure('Visible','off','Color','w','Position',[120 120 860 500]);
cleanup = onCleanup(@() close(f));
a = axes(f,'Position',[0.115 0.16 0.85 0.70],'FontName','Times New Roman',...
    'FontSize',14,'Box','on','TickDir','in','LineWidth',0.75,'XGrid','on',...
    'YGrid','on','GridColor',[0.89 0.89 0.89],'GridAlpha',1,'NextPlot','add');
blue = [0 0.447 0.741];
styles = {'-','--',':','-.'};
markers = {'o','s','^','d'};
lines = gobjects(6,1);
for r = 1:4
    lines(r) = plot(a,speeds,current(:,r),styles{r},'Color',blue*0.82+[.15 .15 .15],...
        'Marker',markers{r},'MarkerSize',4,'LineWidth',1);
end
lines(5) = plot(a,speeds,current(:,5),'--v','Color',[.18 .18 .18],...
    'MarkerSize',4.5,'LineWidth',1.1);
lines(6) = plot(a,speeds,mean(current,2),'-o','Color',[.85 .325 .098],...
    'MarkerFaceColor',[.85 .325 .098],'MarkerSize',4.5,'LineWidth',1.5);
xlim(a,[0 42000]); xticks(a,0:5000:40000); a.XAxis.Exponent = 3;
ylim(a,[104 134]); yticks(a,105:5:130);
xlabel(a,'\fontname{SimSun}速度绝对值（脉冲\fontname{Times New Roman}/s\fontname{SimSun}）','FontSize',15);
ylabel(a,'\fontname{SimSun}双向电流半差（\fontname{Times New Roman}mA\fontname{SimSun}）','FontSize',15);
title(a,'稳态摩擦电流特性','FontName','SimSun','FontSize',18,'FontWeight','normal');
subtitle(a,'\fontname{SimSun}评价位置：\fontname{Times New Roman}10 000–22 000 \fontname{SimSun}脉冲',...
    'FontSize',12,'FontWeight','normal');
labels = compose("\\fontname{SimSun}第\\fontname{Times New Roman} %d \\fontname{SimSun}轮",(1:5)');
labels(5) = labels(5) + "（独立会话）";
legend(a,lines,[labels;"\fontname{SimSun}五轮均值"],'Location','northwest',...
    'NumColumns',2,'Box','off','FontSize',11,'Interpreter','tex');
base = fullfile(root,"data","figures",id+".half_difference");
savefig(f,base+".fig");
exportgraphics(f,base+".svg",'ContentType','vector','BackgroundColor','white');
exportgraphics(f,base+".png",'Resolution',400,'BackgroundColor','white');
end
