using Sharpmake;

[module: Sharpmake.Include("common_project.sharpmake.cs")]

namespace CoroutineStealer
{
    [Sharpmake.Generate]
    class Scheduler : CommonProject
    {
        public string BasePath = @"[project.ProjectBasePath]/scheduler";

        public Scheduler()
        {
            Name = "Scheduler";
            SourceRootPath = "[project.BasePath]/src";
        }

        [Configure()]
        public void Configure(Project.Configuration conf, Target target)
        {
            base.ConfigureAll(conf, target);
            conf.Output = Project.Configuration.OutputType.Lib;
            conf.TargetLibraryPath = "[project.BasePath]/lib/[target.OutputType]";
            conf.SolutionFolder = "lib";
        }
    }
}
