## 新增命令
```
rewrite_share -z -K 100 -S 1 -T 2 -M 1 -R 4(tc_30效果挺好，其他的case也有提升有限，应该是因为窗口太小了，感觉后续可以在refactor命令基础上看看怎么改，refactor窗口天然大些)
aigstore -i 0/1/2 （保存当前AIG网络到内存槽0/1/2，后续可以用aigrestore -a从内存槽恢复）
aigrestore -a 面积优先恢复
```
## 测试表格
```
https://aws128smuu4.feishu.cn/wiki/LQZ3woILLiqsxXk4cgRcLOiUnAg
```
## 评分脚本
```
python score_rank.py a.csv b.csv c.csv

其中csv文件格式如下
case,and_count,lev_count,runtime_sec,extra_peak_rss_mb
tc_public_1,97,9,0.151320,1.320
```
## 批量运行脚本
```
python run_tc_public_abc.py --flow /path/to/flow/script --cases 1-30(也可以1，2，3，5-20写法) --output a.csv
```
