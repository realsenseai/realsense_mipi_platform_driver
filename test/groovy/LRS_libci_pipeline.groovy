import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

pipeline {
	agent { label 'rs-orin-01.realsenseai.com' }

	options {
		timestamps()
		timeout(time: 30, unit: 'MINUTES')
	}

	parameters {
		string(name: 'ARTIFACTORY_SERVER_URL', description: '', defaultValue: 'https://rsartifactory.realsenseai.com/artifactory')
		string(name: 'ARTIFACTORY_REPO', description: '', defaultValue: 'realsense_generic_dev-il-local')
	}

	stages {
		stage('get artifacts') {
			steps {
				script {
					def server = Artifactory.server 'realsense artifacts'
					server.download spec: 'download-spec.json'
						def baseDir = new File('/your/path')
						def newestDir = baseDir.listFiles()
							.findAll { it.isDirectory() }
							.max { it.lastModified() }
				}
			}
		}
		stage('pytest') {
			steps {
				script {
					sh 'pytest --tb=no -s test'
				}
			}
		}
	}
}
