using Sharpmake;

[module: Sharpmake.Include("common_project.sharpmake.cs")]
[module: Sharpmake.Include("../external/externals.sharpmake.cs")]
[module: Sharpmake.Include("scheduler.sharpmake.cs")]

namespace CoroutineStealer
{
    [Sharpmake.Generate]
    class Benchmark : CommonProject
    {
        public string BasePath = @"[project.ProjectBasePath]";
        public Benchmark()
        {
            Name = "Benchmark";
            SourceRootPath = "[project.BasePath]/src";

            IsFileNameToLower = false;
        }

        [Configure()]
        public void Configure(Configuration conf, Target target)
        {
            base.ConfigureAll(conf, target);
            conf.Defines.Add("CATCH_CONFIG_ENABLE_BENCHMARKING");
            conf.AddPublicDependency<externals.Catch2>(target);
            conf.AddPublicDependency<Scheduler>(target);
            conf.Output = Project.Configuration.OutputType.Exe;
        }
    }

    [Sharpmake.Generate]
    public class ExeLibSolution : Sharpmake.Solution
    {
        public ExeLibSolution()
        {
            Name = "CoroutineStealer";

            IsFileNameToLower = false;
            AddTargets(new Target(
                Platform.win64,
                DevEnv.vs2019,
                Optimization.Debug | Optimization.Release));
        }

        [Configure()]
        public void ConfigureAll(Configuration conf, Target target)
        {
            conf.SolutionFileName = "[solution.Name]_[target.DevEnv]_[target.Platform]";
            conf.SolutionPath = @"[solution.SharpmakeCsPath]/projects";
            conf.AddProject<Benchmark>(target);
        }
    }

    public static class main
    {
        [Sharpmake.Main]
        public static void SharpmakeMain(Sharpmake.Arguments arguments)
        {
            arguments.Generate<ExeLibSolution>();
        }
    }
}
