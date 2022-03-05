import seaborn as sb
from matplotlib.lines import Line2D
import pandas as pd
import matplotlib.pyplot as plt

sb.set_style('darkgrid')

#df = pd.DataFrame({"date": ["2018-01-01", "2018-01-02", "2018-01-03", "2018-01-04"],
#                   "column1": [555,525,532,585],
#                   "column2": [50,48,49,51]})
    
#df = pd.DataFrame({"MaxRRPV": ["2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15"],
#                   "column1": [555,525,532,585],
#                   "column2": [50,48,49,51]})

df = pd.read_csv("Book1.csv")#, usecols = ["Trace","Hawkeye", "Ship++"])
#df = pd.melt(df, id_vars = "Counter_size", var_name = "Policy", value_name = "Speedup")
#print(df)

g = sb.lineplot(x=df.Counter_size, y=df.Speedup, color="tab:cyan", marker="o")
sb.lineplot(x=df.Counter_size, y=df.MPKI, color="tab:orange", ax=g.axes.twinx(), marker="o").set(title = "IPC speedup and MPKI for varying counter size")
g.legend(handles=[Line2D([], [], marker='_', color="tab:cyan", label="IPC"), Line2D([], [], marker='_', color="tab:orange", label="MPKI")])

#g.legend(loc = 'upper right')
#g.tight_layout()

#sb.catplot(x = "Trace", y="Speedup", hue = "Policy", data = df, kind = "bar", height=5, aspect=3.8, legend=False).set(title = "Max performance")
#sb.boxplot(x = "Policy", y="Speedup", data = df).set(title = "Max performance")
#plt.xticks(rotation = 45, ha="right", rotation_mode="anchor")

plt.legend(loc='upper right')
plt.tight_layout()

plt.show()
