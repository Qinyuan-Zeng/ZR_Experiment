function plot_prep
d=fileparts(mfilename('fullpath'));
c=readtable(fullfile(d,'phase_counts.csv'),TextType="string");
s=readtable(fullfile(d,'summary.csv'),TextType="string");
names=["startup","accelerate","cruise","brake","post_hold"];
labels=["启动","加速","匀速","减速","停稳保持"];
f=figure(Visible="off",Color="w",Position=[50 50 1100 650]);
tiledlayout(f,2,2,TileSpacing="compact",Padding="compact");
[~,idx]=ismember(names,c.phase);
ax=nexttile; bar(ax,[100*c.mechanism(idx)./c.samples(idx),100*c.candidate(idx)./c.samples(idx)]);
ylabel(ax,'覆盖比例（%）'); title(ax,'各阶段机理与候选标签覆盖');legend(ax,{'可算机理残差','候选训练标签'},Location="best");
fields=["rmse","sd_dev","shift"];
titles=["机理残差幅值","开发段重复性","同工况时段偏移"];
ylabs=["等腿权均方根（raw）","腿均残差标准差中位数（raw）","S2 减 S1 中位数（raw）"];
for j=1:3
    ax=nexttile; z=nan(5,2);
    for k=1:5
        for sign=[1,-1]
            rec=s(s.support=="mechanism" & s.direction==sign & s.phase==names(k),:);
            if height(rec), z(k,1+(sign<0))=rec.(fields(j)); end
        end
    end
    bar(ax,z);ylabel(ax,ylabs(j));title(ax,titles(j));legend(ax,{'正向','反向'},Location="best");
end
for ax=reshape(findall(f,Type="axes"),1,[])
    set(ax,FontName="SimSun",Box="on",TickDir="in",XTick=1:5,XTickLabel=labels,YGrid="on");
end
base=fullfile(d,'residual_diagnostics');savefig(f,[base '.fig']);
exportgraphics(f,[base '.png'],Resolution=400);exportgraphics(f,[base '.svg'],ContentType="vector");close(f);
end
